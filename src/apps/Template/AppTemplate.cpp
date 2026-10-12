/*
MIT License

Copyright (c) 2024 Marek Mach (Bastl Instruments)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/*
 * ATTENSION modifications
 * Copyright (c) 2026 Chris Beattie
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This identifier applies only to Chris Beattie's copyrightable ATTENSION
 * modifications. Original Bastl material remains under the MIT licence above.
 */

#include "AppTemplate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>

#include "common/core/Kastle2.hpp"
#include "common/utils.hpp"
#include "pico/time.h"

using namespace kastle2;


// ------------------------------------------------------------
// MUSICAL LANGUAGE
// ------------------------------------------------------------

static constexpr int kMajorScale[7] = {
    0, 2, 4, 5, 7, 9, 11
};

static constexpr int kRootMidiNote = 60;

namespace
{
struct EndingPlan
{
    uint8_t kick[3];
    uint8_t snare[3];
    uint8_t bars;
};

// The first five are concise, authored landing bars.  The next two are
// statement/answer pairs; the final plan is deliberately left undescribed
// here beyond its three-bar shape so it remains a performance discovery.
static constexpr EndingPlan kEndingPlans[] = {
    {{0x89, 0, 0}, {0xa0, 0, 0}, 1},
    {{0x91, 0, 0}, {0xc4, 0, 0}, 1},
    {{0xa9, 0, 0}, {0xd0, 0, 0}, 1},
    {{0xb1, 0, 0}, {0xc8, 0, 0}, 1},
    {{0xb5, 0, 0}, {0xe8, 0, 0}, 1},
    {{0x11, 0xa9, 0}, {0x40, 0xd0, 0}, 2},
    {{0x25, 0x99, 0}, {0x48, 0xe0, 0}, 2},
    {{0x15, 0x2b, 0xd5}, {0x42, 0x94, 0xf0}, 3},
};
static constexpr uint8_t kSecretEndingPlan = 7;

int MapVoiceDialSelection(const int32_t raw_value,
                          const int previous_selection,
                          const bool apply_hysteresis)
{
    static constexpr std::array<int32_t, 11> kStarts = {
        0, 82, 483, 884, 1286, 1687, 2089, 2490, 2891, 3293, 3694};
    static constexpr int32_t kBoundaryResistance = 20;

    const int32_t value = std::clamp<int32_t>(raw_value, POT_MIN, POT_MAX);
    int selection = 10;
    for (int candidate = 1; candidate <= 10; ++candidate)
    {
        if (value < kStarts[candidate])
        {
            selection = candidate - 1;
            break;
        }
    }

    if (!apply_hysteresis)
        return selection;

    const int previous = std::clamp(previous_selection, 0, 10);
    if (selection > previous &&
        value < kStarts[previous + 1] + kBoundaryResistance)
        return previous;
    if (selection < previous &&
        value > kStarts[previous] - kBoundaryResistance)
        return previous;
    return selection;
}
}


// ------------------------------------------------------------
// SCALE DEGREE -> FREQUENCY
// ------------------------------------------------------------

static int ScaleDegreeToMidiNote(int scale_degree)
{
    // Floor division keeps the diatonic scale correct below degree zero.
    int octave = scale_degree / 7;
    int degree_in_scale = scale_degree % 7;
    if (degree_in_scale < 0)
    {
        degree_in_scale += 7;
        --octave;
    }

    return
        kRootMidiNote +
        (octave * 12) +
        kMajorScale[degree_in_scale];
}

static float ScaleDegreeToFrequency(int scale_degree)
{
    const int midi_note = ScaleDegreeToMidiNote(scale_degree);

    return 440.0f *
           std::pow(
               2.0f,
               (midi_note - 69) / 12.0f);
}


// ------------------------------------------------------------
// CHROMATIC TENSION OFFSET
// ------------------------------------------------------------
//
// The composition brain normally thinks in scale degrees. Tension needs
// access to a small amount of chromatic friction without changing HOME's
// diatonic language, so this helper offsets an already-valid scale tone
// by an exact number of semitones at the final sound boundary.

static float ChromaticOffsetFrequency(
    int scale_degree,
    int semitone_offset)
{
    return ScaleDegreeToFrequency(scale_degree) *
           std::pow(
               2.0f,
               semitone_offset / 12.0f);
}


// ------------------------------------------------------------
// PITCH SAFETY
// ------------------------------------------------------------

int AppTemplate::ClampPitchDegree(int degree) const
{
    if (degree < kHomePitchMinimum)
    {
        return kHomePitchMinimum;
    }

    if (degree > kHomePitchMaximum)
    {
        return kHomePitchMaximum;
    }

    return degree;
}



float AppTemplate::PotToScalePosition(int32_t value)
{
    const int32_t bounded = std::max<int32_t>(POT_MIN,
        std::min<int32_t>(value, POT_MAX));
    return kHomePitchMinimum + static_cast<float>(bounded - POT_MIN) *
        (kPitchPositions - 1) / static_cast<float>(POT_MAX - POT_MIN);
}

float AppTemplate::ReferenceFrequency(float position) const
{
    return ScaleDegreeToFrequency(
        ClampPitchDegree(static_cast<int>(std::round(position))));
}

void AppTemplate::EnforceRhythmGrammar()
{
    // Keep the evolved motif. Restore at most one lost rhythmic landmark;
    // never rewrite its pitches or roll a new attack pattern here.
    const auto restore = [this](size_t step) {
        if (!current_rhythm_[step]) current_rhythm_[step] = true;
    };
    const auto count = [this]() {
        return std::count(current_rhythm_.begin(), current_rhythm_.end(), true);
    };
    switch (home_family_)
    {
    case 0: // Sparse: leave room around its two inherited landmarks.
        if (count() > 3)
            for (size_t i : {size_t(1), size_t(5), size_t(3), size_t(7), size_t(2), size_t(6)})
                if (current_rhythm_[i] && !home_rhythm_[i]) { current_rhythm_[i] = false; break; }
        break;
    case 1: // Steady: keep a regular pulse audible if it was lost.
        if (!current_rhythm_[0] && !current_rhythm_[2] &&
            !current_rhythm_[4] && !current_rhythm_[6]) restore(4);
        break;
    case 2: // Call and Answer: each inherited half gets a voice.
        if (!current_rhythm_[0] && !current_rhythm_[1] &&
            !current_rhythm_[2] && !current_rhythm_[3]) restore(0);
        else if (!current_rhythm_[4] && !current_rhythm_[5] &&
                 !current_rhythm_[6] && !current_rhythm_[7]) restore(4);
        break;
    case 3: // 3+3+2: keep one of the two later group onsets.
        if (!current_rhythm_[3] && !current_rhythm_[6]) restore(3);
        break;
    case 4: // Swing character lives in HomeStepDelay, not in pitch.
        break;
    case 5: // Pickup: preserve the safe, delayed boundary approach.
        restore(7);
        break;
    case 6: // Broken: retain a gap if mutation filled every step.
        if (count() == kPhraseLength) current_rhythm_[2] = false;
        break;
    case 7: // Urgent: retain propulsion without a sub-step retrigger.
        if (count() < 5) restore(4);
        break;
    }
}

// ------------------------------------------------------------
// ORGANIC TIMING / NOTE SCHEDULER
// ------------------------------------------------------------

int AppTemplate::MillisecondsToSamples(
    int milliseconds) const
{
    return static_cast<int>(
        (static_cast<int64_t>(SAMPLE_RATE) *
         milliseconds) /
        1000);
}

float AppTemplate::VoiceARegisterMultiplier(
    HomePresentationRegister presentation) const
{
    switch (presentation)
    {
    case HomePresentationRegister::LOW: return 0.5f;
    case HomePresentationRegister::HIGH: return 2.0f;
    default: return 1.0f;
    }
}

void AppTemplate::ClearStructuredHomePresentation()
{
    structured_home_length_ = 0;
    structured_home_phrase_index_ = 0;
    structured_home_pattern_ = 0;
    home_presentation_register_ = HomePresentationRegister::NORMAL;
}

void AppTemplate::BeginStructuredHomePresentation(uint8_t length)
{
    structured_home_length_ = length;
    structured_home_phrase_index_ = 0;
    if (length == 2)
        structured_home_pattern_ = next_home_two_phrase_pattern_++ % 5;
    else if (length == 3)
        structured_home_pattern_ = next_home_three_phrase_pattern_++ % 6;
    else
        structured_home_pattern_ = next_home_four_phrase_pattern_++ % 6;
    AdvanceStructuredHomePresentation();
    structured_home_phrase_index_ = 0;
}

void AppTemplate::AdvanceStructuredHomePresentation()
{
    static constexpr HomePresentationRegister kTwoPhrasePatterns[5][2] = {
        {HomePresentationRegister::NORMAL, HomePresentationRegister::NORMAL},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::LOW},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH},
        {HomePresentationRegister::LOW, HomePresentationRegister::HIGH},
        {HomePresentationRegister::HIGH, HomePresentationRegister::LOW},
    };
    static constexpr HomePresentationRegister kThreePhrasePatterns[6][3] = {
        {HomePresentationRegister::NORMAL, HomePresentationRegister::LOW,
         HomePresentationRegister::NORMAL},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH,
         HomePresentationRegister::NORMAL},
        {HomePresentationRegister::LOW, HomePresentationRegister::NORMAL,
         HomePresentationRegister::LOW},
        {HomePresentationRegister::HIGH, HomePresentationRegister::NORMAL,
         HomePresentationRegister::HIGH},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::LOW,
         HomePresentationRegister::HIGH},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH,
         HomePresentationRegister::LOW},
    };
    static constexpr HomePresentationRegister kFourPhrasePatterns[6][4] = {
        {HomePresentationRegister::NORMAL, HomePresentationRegister::NORMAL,
         HomePresentationRegister::LOW, HomePresentationRegister::NORMAL},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::NORMAL,
         HomePresentationRegister::HIGH, HomePresentationRegister::NORMAL},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::LOW,
         HomePresentationRegister::NORMAL, HomePresentationRegister::LOW},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH,
         HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::LOW,
         HomePresentationRegister::HIGH, HomePresentationRegister::NORMAL},
        {HomePresentationRegister::NORMAL, HomePresentationRegister::HIGH,
         HomePresentationRegister::LOW, HomePresentationRegister::NORMAL},
    };

    if (structured_home_length_ == 2)
        home_presentation_register_ =
            kTwoPhrasePatterns[structured_home_pattern_][structured_home_phrase_index_];
    else if (structured_home_length_ == 3)
        home_presentation_register_ =
            kThreePhrasePatterns[structured_home_pattern_][structured_home_phrase_index_];
    else if (structured_home_length_ == 4)
        home_presentation_register_ =
            kFourPhrasePatterns[structured_home_pattern_][structured_home_phrase_index_];
}


void AppTemplate::SchedulePhraseNote(
    size_t step,
    bool home_plan,
    bool suppress_harmony)
{
    pending_pickup_ = home_family_ == 5 && step == 7;
    pending_note_is_resolve_ = tension_phase_ == TensionPhase::RESOLVE;
    pending_degree_ = home_plan ? home_pitch_[step] : current_pitch_[step];
    pending_voice_a_register_ = home_plan && home_refrain_active_ ?
        home_presentation_register_ : !home_plan ? ordinary_wander_register_ :
        HomePresentationRegister::NORMAL;

    pending_note_delay_samples_ =
        home_plan ? home_timing_samples_[step] : current_timing_samples_[step];
    pending_note_delay_samples_ += HomeStepDelay(step);
    // A pending note belongs to this clock step. An early next clock
    // discards it (except the deliberate step-7 pickup below).
    if (last_clock_interval_samples_ > MillisecondsToSamples(12))
        pending_note_delay_samples_ = std::min(
            pending_note_delay_samples_,
            last_clock_interval_samples_ - MillisecondsToSamples(8));
    if (step == 0)
        pending_note_delay_samples_ += pickup_recovery_samples_;
    pickup_recovery_samples_ = 0;

    if (pending_note_delay_samples_ < 0)
        pending_note_delay_samples_ = 0;

    pending_harmony_ = false;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;


    // WANDER shapes the probability tiers but does not make Voice B
    // unavailable at HOME. TENSION phrase participation remains the
    // arrangement-level eligibility decision.
    int harmony_chance = 30;
    if (current_distance_ >= 10)
        harmony_chance = 90;
    else if (current_distance_ >= 7)
        harmony_chance = 75;
    else if (current_distance_ >= 4)
        harmony_chance = 55;
    if (!suppress_harmony && voice_b_lab_selection_ > 0 &&
        voice_b_phrase_participating_ &&
        voice_b_arrangement_ != VoiceBArrangement::HOLD &&
        harmony_chance > 0)
    {
        // Keep the ordinary chance roll at locked TENSION. At 9 and 10, B
        // follows every legitimate Voice A attack instead of declining it.
        const int harmony_roll = std::rand() % 100;
        if (tension_amount_ >= 9 || harmony_roll < harmony_chance)
        {
            pending_harmony_ = true;

            if (tension10_chromatic_lock_)
            {
                pending_harmony_chromatic_ = true;
                if (tension_phase_ == TensionPhase::HOLD &&
                    flourish_rub_pending_)
                    flourish_rub_pending_ = false;
            }
            else
            {
                pending_harmony_degree_ =
                    ChooseHarmonyDegree(
                        ClampPitchDegree(pending_degree_));

                // Tension is harmonic instability, not random ugliness.
                // BUILD leans into a neighboring scale tone. HOLD occasionally
                // introduces a true chromatic semitone rub. RESOLVE deliberately
                // returns Voice B to a stable fifth.
                if (tension_phase_ == TensionPhase::BUILD)
                {
                    pending_harmony_degree_ =
                        ClampPitchDegree(ClampPitchDegree(pending_degree_) + 1);
                }
                else if (tension_phase_ == TensionPhase::HOLD)
                {
                    // The semitone rub is now an accent, not the default sound.
                    int chromatic_chance =
                        12 + (tension_amount_ * 5);

                    pending_harmony_chromatic_ = flourish_rub_pending_ ||
                        (std::rand() % 100) < chromatic_chance;
                    if (flourish_rub_pending_)
                        flourish_rub_pending_ = false;

                    if (!pending_harmony_chromatic_)
                    {
                        pending_harmony_degree_ =
                            ClampPitchDegree(ClampPitchDegree(pending_degree_) + 1);
                    }
                }
                else if (tension_phase_ == TensionPhase::RESOLVE)
                {
                    pending_harmony_degree_ =
                        ClampPitchDegree(ClampPitchDegree(pending_degree_) + 4);
                }
            }
        }
    }


    pending_note_ = true;
}

void AppTemplate::UpdateVoiceBPhraseParticipation()
{
    // This private xorshift stream deliberately never consumes std::rand(),
    // preserving the existing melody, harmony, and percussion sequence.
    auto next = [this]() -> uint32_t {
        voice_b_participation_rng_ ^= voice_b_participation_rng_ << 13;
        voice_b_participation_rng_ ^= voice_b_participation_rng_ >> 17;
        voice_b_participation_rng_ ^= voice_b_participation_rng_ << 5;
        return voice_b_participation_rng_;
    };
    const bool was_participating = voice_b_phrase_participating_;
    const auto enter_rest_if_needed = [this, was_participating]() {
        if (was_participating && !voice_b_phrase_participating_)
            EnterVoiceBRest();
    };
    const int t = std::clamp(tension_amount_, 0, 10);
    const bool tension_changed = voice_b_participation_tension_ >= 0 &&
        voice_b_participation_tension_ != t;
    voice_b_participation_tension_ = t;

    if (t == 5)
    {
        voice_b_phrase_participating_ = true;
        voice_b_participation_phrases_remaining_ = 255;
        voice_b_participation_initialized_ = true;
        enter_rest_if_needed();
        return;
    }

    if (t >= 9)
    {
        voice_b_phrase_participating_ = true;
        voice_b_participation_phrases_remaining_ = 255;
        voice_b_participation_initialized_ = true;
        // TENSION 9-10 are continuous FOLLOW: end an existing HOLD without
        // releasing its sounding pitch.
        if (voice_b_arrangement_ == VoiceBArrangement::HOLD)
        {
            voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
            voice_b_hold_phrases_remaining_ = 0;
        }
        enter_rest_if_needed();
        return;
    }
    static constexpr uint8_t kOnMin[]  = {2,2,2,2,4,5,6,7,6};
    static constexpr uint8_t kOnMax[]  = {2,2,3,3,6,7,8,9,10};
    static constexpr uint8_t kOffMin[] = {3,3,2,2,1,1,1,1,1};
    static constexpr uint8_t kOffMax[] = {5,4,4,3,3,2,2,1,1};

    if (tension_changed)
    {
        // Ordinary changes alter the rules without commanding an immediate
        // appearance or disappearance. Keep the current state and retain
        // any compatible remainder; only cap an overlong run for the new
        // setting. The normal expiry path remains the only REST transition.
        const uint8_t maximum = voice_b_phrase_participating_ ?
            kOnMax[t] : kOffMax[t];
        voice_b_participation_phrases_remaining_ = std::min(
            voice_b_participation_phrases_remaining_, maximum);
        return;
    }

    if (!voice_b_participation_initialized_)
    {
        voice_b_participation_initialized_ = true;
        voice_b_phrase_participating_ = false;
        voice_b_participation_phrases_remaining_ =
            kOffMin[t] + next() % (kOffMax[t] - kOffMin[t] + 1);
        enter_rest_if_needed();
        return;
    }
    if (voice_b_participation_phrases_remaining_ > 0)
        --voice_b_participation_phrases_remaining_;
    if (voice_b_participation_phrases_remaining_ == 0)
    {
        voice_b_phrase_participating_ = !voice_b_phrase_participating_;
        const uint8_t lo = voice_b_phrase_participating_ ? kOnMin[t] : kOffMin[t];
        const uint8_t hi = voice_b_phrase_participating_ ? kOnMax[t] : kOffMax[t];
        voice_b_participation_phrases_remaining_ = lo + next() % (hi - lo + 1);
    }
    enter_rest_if_needed();
}

bool AppTemplate::VoiceBSupportsHold() const
{
    if (voice_b_lab_selection_ < 1 || voice_b_lab_selection_ > kVoicePositions)
        return false;
    switch (static_cast<VoiceCharacter>(voice_b_lab_selection_ - 1))
    {
    case VoiceCharacter::SOFT:
    case VoiceCharacter::ORGAN:
    case VoiceCharacter::SYNTH:
    case VoiceCharacter::STRINGS:
    case VoiceCharacter::REED:
        return true;
    default:
        return false;
    }
}

void AppTemplate::EnterVoiceBRest()
{
    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
    voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
    voice_b_hold_phrases_remaining_ = 0;
    voice_b_has_harmony_note_ = false;
    voice_b_hold_eligible_ = false;
    pending_harmony_ = false;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;
    harmony_env_.Release();
}

void AppTemplate::UpdateVoiceBHoldForPhrase()
{
    static constexpr uint8_t kHoldChance[] = {
        10, 10, 10, 25, 45, 100, 45, 30, 15, 0, 0};
    if (!voice_b_phrase_participating_)
        return;

    // TENSION 9-10 mean continuous FOLLOW. An adoption at this boundary has
    // already returned an active HOLD to FOLLOW without a release.
    if (tension_amount_ >= 9)
        return;

    if (voice_b_arrangement_ == VoiceBArrangement::HOLD)
    {
        if (voice_b_hold_phrases_remaining_ > 0)
            --voice_b_hold_phrases_remaining_;
        if (voice_b_hold_phrases_remaining_ == 0)
            voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
        return;
    }

    if (!voice_b_hold_eligible_ || !voice_b_has_harmony_note_ ||
        !VoiceBSupportsHold())
        return;

    auto next = [this]() -> uint32_t {
        voice_b_hold_rng_ ^= voice_b_hold_rng_ << 13;
        voice_b_hold_rng_ ^= voice_b_hold_rng_ >> 17;
        voice_b_hold_rng_ ^= voice_b_hold_rng_ << 5;
        return voice_b_hold_rng_;
    };
    const int t = std::clamp(tension_amount_, 0, 10);
    if (next() % 100 >= kHoldChance[t])
        return;

    voice_b_arrangement_ = VoiceBArrangement::HOLD;
    voice_b_hold_eligible_ = false;
    const uint32_t duration_roll = next() % 20;
    if (t == 5)
    {
        // TENSION 5 keeps its guaranteed harmonic anchor brief: 85% one
        // phrase and 15% two phrases, without three-phrase holds.
        voice_b_hold_phrases_remaining_ = duration_roll < 17 ? 1 : 2;
    }
    else
    {
        voice_b_hold_phrases_remaining_ = duration_roll < 12 ? 1 :
            duration_roll < 17 ? 2 : 3;
    }
}


void AppTemplate::TriggerPendingNote()
{
    if (pending_note_is_resolve_ &&
        tension_phase_ == TensionPhase::RESOLVE)
    {
        if (voice_b_resolve_suspension_ ==
            VoiceBResolveSuspension::AWAITING_FIRST_A_ATTACK)
        {
            // A establishes RESOLVE while the already-sounding B is left
            // untouched. Scheduling and its existing RNG draws have run;
            // discard only this pending B attack.
            pending_harmony_ = false;
            pending_harmony_degree_ = 0;
            pending_harmony_chromatic_ = false;
            voice_b_resolve_suspension_ =
                VoiceBResolveSuspension::AWAITING_SECOND_A_ATTACK;
        }
        else if (voice_b_resolve_suspension_ ==
                 VoiceBResolveSuspension::AWAITING_SECOND_A_ATTACK)
        {
            // The next actual A attack permits the normal current-A RESOLVE
            // move; A1 will record it only if B really triggers.
            voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
        }
    }

    atmosphere_previous_degree_ = atmosphere_recent_degree_;
    atmosphere_recent_degree_ = ClampPitchDegree(pending_degree_);
    if (!temporary_voice_a_)
        active_voice_a_ = selected_voice_a_;
    if (!temporary_voice_b_)
        active_voice_b_ = selected_voice_b_;

    if (shift_audition_ && reference_audition_)
        active_voice_a_ = VoiceCharacter::SOFT;
    else if (shift_audition_ && !reference_audition_ && voice_a_lab_off_)
    {
        // SHIFT/HOME is an explicit presentation request: borrow B's
        // adopted character without starting or changing Voice B itself.
        const int b_presentation_selection = voice_b_lab_selection_ > 0 ?
            (voice_b_lab_active_selection_ > 0 ? voice_b_lab_active_selection_ :
             voice_b_lab_selection_) : 0;
        active_voice_a_ = b_presentation_selection > 0 ?
            static_cast<VoiceCharacter>(b_presentation_selection - 1) :
            VoiceCharacter::SOFT;
    }
    SetVoiceACharacter(active_voice_a_);
    voice_b_lab_off_ = (voice_b_lab_selection_ == 0);
    if (voice_b_lab_off_)
    {
        voice_b_lab_active_selection_ = 0;
    }
    else if (pending_harmony_)
    {
        voice_b_lab_active_selection_ = voice_b_lab_selection_;
        const float harmony_frequency = pending_harmony_chromatic_ ?
            ChromaticOffsetFrequency(ClampPitchDegree(pending_degree_), 1) :
            ScaleDegreeToFrequency(pending_harmony_degree_);
        ConfigureVoiceBLab(voice_b_lab_active_selection_, harmony_frequency);
        voice_b_has_harmony_note_ = true;
        voice_b_hold_eligible_ = true;
    }

    const float note_frequency = (reference_audition_ ? reference_frequency_ :
        ScaleDegreeToFrequency(ClampPitchDegree(pending_degree_))) *
        VoiceARegisterMultiplier(pending_voice_a_register_);
    // WHISTLE follows each legitimate melodic attack at a bright, related
    // interval. Its phase keeps running in the audio path for click-free
    // ATMOS crossfades; this only changes the control-rate pitch target.
    const int whistle_degree = ClampPitchDegree(pending_degree_) + 11;
    if (!ending_cadence_active_ &&
        (!voice_a_lab_off_ ||
         (voice_b_lab_selection_ > 0 && pending_harmony_)))
    {
        atmosphere_whistle_osc_.SetFrequency(ScaleDegreeToFrequency(whistle_degree));
        atmosphere_whistle_level_ = Q15_MAX;
    }
    const auto &voice_a_definition =
        kVoicePalette[static_cast<size_t>(active_voice_a_)];
    voice_a_layer_level_ = static_cast<q15_t>(
        voice_a_definition.layer_level * Q15_MAX);
    SetVoiceAOutputLevel(active_voice_a_);
    voice_a_uses_fm_ = voice_a_definition.fm_index > 0.0f;
    SetDreamTargets(note_frequency, voice_a_definition.layer_ratio,
        dream_a_current_, dream_a_target_,
        dream_a_layer_current_, dream_a_layer_target_, dream_a_ready_);
    osc_.SetNativeFrequency(dream_a_current_);
    voice_a_layer_osc_.SetNativeFrequency(dream_a_layer_current_);
    voice_a_fm_.SetNativeFrequency(dream_a_current_);
    if (active_voice_a_ == VoiceCharacter::ODDITY)
    {
        voice_a_oddity_glint_.SetNativeFrequency(
            freq_to_q31(note_frequency * 3.0f, SAMPLE_RATE));
        voice_a_oddity_glint_delay_ = 968;      // 22 ms at 44 kHz.
        voice_a_oddity_glint_remaining_ = 2288; // 52 ms at 44 kHz.
        voice_a_oddity_glint_ramp_ = 0;
        voice_a_oddity_glint_envelope_ = 0;
    }

    env_.Trigger();

    if (pending_harmony_ && !voice_b_lab_off_)
    {
        // ConfigureVoiceBLab has already applied the candidate's pitch ratios.
        harmony_env_.Trigger();
        // Store the final B pitch only when its audible event has been triggered.
        last_voice_b_midi_note_ = pending_harmony_chromatic_ ?
            ScaleDegreeToMidiNote(ClampPitchDegree(pending_degree_)) + 1 :
            ScaleDegreeToMidiNote(pending_harmony_degree_);
    }
    StartFractureGate();


    pending_note_ = false;
    pending_note_is_resolve_ = false;
    pending_pickup_ = false;
    pending_note_delay_samples_ = 0;
    pending_harmony_ = false;
    pending_harmony_chromatic_ = false;
}


void AppTemplate::StartFractureGate()
{
    atmosphere_fracture_gate_level_ = Q15_MAX;
    atmosphere_fracture_gate_floor_ = Q15_MAX;
    atmosphere_fracture_gate_age_ = 0;
    atmosphere_fracture_gate_cut_start_ = 0;
    atmosphere_fracture_gate_release_ = 0;
    atmosphere_fracture_gate_decrement_ = 0;
    atmosphere_fracture_gate_active_ = false;

    const uint32_t intensity = static_cast<uint32_t>(atmosphere_effect_strength_);
    atmosphere_event_state_ ^= atmosphere_event_state_ << 13;
    atmosphere_event_state_ ^= atmosphere_event_state_ >> 17;
    atmosphere_event_state_ ^= atmosphere_event_state_ << 5;
    const uint32_t severity = intensity > Q15_MAX ? Q15_MAX : intensity;
    const int32_t timing_variation =
        static_cast<int32_t>((atmosphere_event_state_ >> 8) & 3u) - 1;
    // Compress the existing useful lower half of the duration curve across
    // the full TENSION travel. Raw midpoint TENSION produces 52.5% effect
    // strength through the smooth TENSION mapping.
    const uint32_t minimum_onset = MillisecondsToSamples(30) +
        ((MillisecondsToSamples(90) * (Q15_MAX - Q15_MAX / 20)) >> 15);
    const uint32_t midpoint_strength = 17201;
    const uint32_t midpoint_onset = MillisecondsToSamples(30) +
        ((MillisecondsToSamples(90) * (Q15_MAX - midpoint_strength)) >> 15);
    const uint32_t base_onset = midpoint_onset +
        ((minimum_onset - midpoint_onset) * (Q15_MAX - severity) /
            (Q15_MAX - Q15_MAX / 20));
    const int32_t onset = std::max<int32_t>(
        MillisecondsToSamples(8),
        static_cast<int32_t>(base_onset) +
            timing_variation * MillisecondsToSamples(1));
    const uint32_t release = MillisecondsToSamples(6);
    atmosphere_fracture_gate_cut_start_ = onset;
    atmosphere_fracture_gate_release_ = std::max<uint32_t>(1, release);
    atmosphere_fracture_gate_floor_ = Q15_ZERO;
    atmosphere_fracture_gate_decrement_ =
        (Q15_MAX - atmosphere_fracture_gate_floor_) /
        static_cast<int32_t>(atmosphere_fracture_gate_release_);
    if (atmosphere_fracture_gate_decrement_ < 1)
        atmosphere_fracture_gate_decrement_ = 1;
    atmosphere_fracture_gate_active_ = true;
}


// ------------------------------------------------------------
// HARMONY VOICE
// ------------------------------------------------------------
//
// Voice B chooses a scale-aware partner for each sounding melody note.
//
// The interval is expressed in SCALE DEGREES, not semitones:
// +2 = diatonic third
// +4 = diatonic fifth
// +7 = octave
//
// If an upward harmony would leave Attension's current two-octave
// pitch territory, the voice moves below the melody instead.

int AppTemplate::ChooseHarmonyDegree(int melody_degree) const
{
    constexpr std::array<int, 3> kIntervals = {2, 4, 7};
    constexpr std::array<int, 3> kBaseWeights = {60, 30, 10};
    std::array<int, 3> candidates{};
    std::array<int, 3> weights = kBaseWeights;
    std::array<int, 3> midi_notes{};

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        int candidate = melody_degree + kIntervals[i];
        if (candidate >= kPitchPositions)
            candidate = melody_degree - kIntervals[i];
        candidates[i] = ClampPitchDegree(candidate);
        midi_notes[i] = ScaleDegreeToMidiNote(candidates[i]);
    }

    // A1 is a soft preference for the nearest final audible B pitch, rather
    // than deterministic voice leading. Identical clamped outcomes receive
    // the same bias, preserving their combined nominal weight.
    if (last_voice_b_midi_note_ >= 0)
    {
        int nearest_distance = std::abs(midi_notes[0] - last_voice_b_midi_note_);
        for (size_t i = 1; i < midi_notes.size(); ++i)
            nearest_distance = std::min(nearest_distance,
                std::abs(midi_notes[i] - last_voice_b_midi_note_));
        for (size_t i = 0; i < weights.size(); ++i)
            if (std::abs(midi_notes[i] - last_voice_b_midi_note_) == nearest_distance)
                weights[i] *= 2;
    }

    int total_weight = 0;
    for (int weight : weights)
        total_weight += weight;
    const int roll = std::rand() % total_weight;
    int accumulated_weight = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        accumulated_weight += weights[i];
        if (roll < accumulated_weight)
            return candidates[i];
    }
    return candidates.back();
}


// ------------------------------------------------------------
// VOICE CHARACTER PALETTE
// ------------------------------------------------------------
//
// Manual character selections persist across phrases.

void AppTemplate::ConfigureVoice(
    VoiceCharacter character,
    Oscillator &oscillator,
    Oscillator &layer_oscillator,
    Fm2 &fm,
    AdsrEnv &envelope)
{
    const auto &definition =
        kVoicePalette[static_cast<size_t>(character)];

    oscillator.SetWaveform(definition.waveform);
    oscillator.SetAmplitude(Q31_MAX);
    oscillator.SetPulseWidth(q31(0.5f));

    layer_oscillator.SetWaveform(definition.layer_waveform);
    layer_oscillator.SetAmplitude(Q31_MAX);
    layer_oscillator.SetPulseWidth(q31(0.5f));

    envelope.SetAttackTime(definition.attack);
    envelope.SetSustainLevel(definition.sustain >= 1.0f ? Q31_MAX :
        float_to_q31(definition.sustain));
    envelope.SetDecayTime(definition.decay);

    fm.SetRatio(
        static_cast<q31_t>(definition.fm_ratio * Q31_MAX));
    fm.SetIndex(
        static_cast<q31_t>(definition.fm_index * Q31_MAX));
}

void AppTemplate::SetVoiceACharacter(VoiceCharacter character)
{
    ConfigureVoice(character, osc_, voice_a_layer_osc_, voice_a_fm_, env_);
}

void AppTemplate::SetVoiceBCharacter(VoiceCharacter character)
{
    ConfigureVoice(character, harmony_osc_, voice_b_layer_osc_, voice_b_fm_, harmony_env_);
}


void AppTemplate::UpdateVoiceCharactersForPhrase()
{
    // Preserve the manually selected character for each voice.
    active_voice_a_ = selected_voice_a_;
    active_voice_b_ = selected_voice_b_;
    temporary_voice_a_ = false;
    temporary_voice_b_ = false;
}


// ------------------------------------------------------------
// TENSION ARC
// ------------------------------------------------------------
//
// TENSION and WANDER are deliberately independent dimensions. WANDER
// controls familiarity; TENSION controls harmonic stability. TIME sets
// how readily an arc begins and how strongly HOLD is allowed to bite.
//
// Each arc is phrase-shaped: STABLE -> BUILD -> HOLD -> RESOLVE -> STABLE.
// Resolution is an active musical event, not merely the absence of
// dissonance. Exact HOME always cancels the arc.

int32_t AppTemplate::MapPatchCvAroundPot(int32_t cv, int32_t physical_value)
{
    cv = std::clamp<int32_t>(cv, ADC_0V, ADC_5V);
    physical_value = std::clamp<int32_t>(physical_value, POT_MIN, POT_MAX);
    constexpr int32_t midpoint = ADC_5V / 2;
    if (cv <= midpoint)
    {
        return static_cast<int32_t>((static_cast<int64_t>(physical_value) * cv) /
                                    midpoint);
    }
    return physical_value + static_cast<int32_t>(
        (static_cast<int64_t>(POT_MAX - physical_value) * (cv - midpoint)) /
        (ADC_5V - midpoint));
}

int32_t AppTemplate::UpdatePatchMod(PatchModState &state, int32_t cv,
                                    int32_t physical_value, uint32_t now_ms,
                                    bool smooth)
{
    cv = std::clamp<int32_t>(cv, ADC_0V, ADC_5V);
    physical_value = std::clamp<int32_t>(physical_value, POT_MIN, POT_MAX);
    if (state.window_started_ms == 0)
    {
        state.window_started_ms = now_ms;
        state.window_min = state.window_max = cv;
        state.slow_window_started_ms = now_ms;
        state.slow_window_min = state.slow_window_max = cv;
        state.filtered_value = physical_value;
    }
    state.window_min = std::min(state.window_min, cv);
    state.window_max = std::max(state.window_max, cv);
    state.slow_window_min = std::min(state.slow_window_min, cv);
    state.slow_window_max = std::max(state.slow_window_max, cv);

    if (now_ms - state.window_started_ms >= kPatchObserveWindowMs)
    {
        if (state.window_max - state.window_min >= kPatchActivityThreshold)
        {
            state.last_activity_ms = now_ms;
            if (!state.active && ++state.qualifying_windows >= kPatchActivationWindows)
            {
                state.active = true;
                state.filtered_value = physical_value;
                state.slow_qualifying_windows = 0;
            }
        }
        else if (!state.active)
        {
            state.qualifying_windows = 0;
        }
        state.window_min = state.window_max = cv;
        state.window_started_ms = now_ms;
    }

    if (now_ms - state.slow_window_started_ms >= kPatchSlowObserveWindowMs)
    {
        if (state.slow_window_max - state.slow_window_min >= kPatchSlowActivityThreshold)
        {
            state.last_activity_ms = now_ms;
            if (!state.active &&
                ++state.slow_qualifying_windows >= kPatchSlowActivationWindows)
            {
                state.active = true;
                state.filtered_value = physical_value;
                state.qualifying_windows = 0;
            }
        }
        else if (!state.active)
        {
            state.slow_qualifying_windows = 0;
        }
        state.slow_window_min = state.slow_window_max = cv;
        state.slow_window_started_ms = now_ms;
    }

    if (state.active && now_ms - state.last_activity_ms >= kPatchInactivityTimeoutMs)
    {
        state.active = false;
        state.qualifying_windows = 0;
        state.recenter_from = state.filtered_value;
        state.recenter_started_ms = now_ms;
    }

    if (state.active)
    {
        const int32_t mapped = MapPatchCvAroundPot(cv, physical_value);
        if (smooth)
        {
            const int32_t delta = mapped - state.filtered_value;
            int32_t change = delta / kContinuousPatchSmoothingDivisor;
            if (change == 0 && delta != 0)
                change = delta > 0 ? 1 : -1;
            state.filtered_value += change;
        }
        else
            state.filtered_value = mapped;
        return state.filtered_value;
    }

    if (state.recenter_started_ms != 0)
    {
        const uint32_t elapsed = now_ms - state.recenter_started_ms;
        if (elapsed < kPatchRecenterMs)
        {
            return state.recenter_from + static_cast<int32_t>(
                (static_cast<int64_t>(physical_value - state.recenter_from) * elapsed) /
                kPatchRecenterMs);
        }
        state.recenter_started_ms = 0;
    }
    return physical_value;
}

bool AppTemplate::DetectManualPatchTakeover(int32_t physical_value,
                                            int32_t &previous_value)
{
    physical_value = std::clamp<int32_t>(physical_value, POT_MIN, POT_MAX);
    if (previous_value < 0)
    {
        previous_value = physical_value;
        return false;
    }
    if (std::abs(physical_value - previous_value) < kPatchManualTakeoverThreshold)
        return false;
    previous_value = physical_value;
    return true;
}

void AppTemplate::ReleasePatchMod(PatchModState &state, int32_t cv,
                                  int32_t physical_value, uint32_t now_ms)
{
    cv = std::clamp<int32_t>(cv, ADC_0V, ADC_5V);
    physical_value = std::clamp<int32_t>(physical_value, POT_MIN, POT_MAX);
    state.active = false;
    state.qualifying_windows = 0;
    state.slow_qualifying_windows = 0;
    state.window_min = state.window_max = cv;
    state.slow_window_min = state.slow_window_max = cv;
    state.filtered_value = physical_value;
    state.recenter_from = physical_value;
    state.window_started_ms = now_ms;
    state.slow_window_started_ms = now_ms;
    state.last_activity_ms = 0;
    state.recenter_started_ms = 0;
}

int AppTemplate::MapControlToPosition(int32_t value, int positions)
{
    value = std::clamp<int32_t>(value, POT_MIN, POT_MAX);
    return static_cast<int>((static_cast<int64_t>(value - POT_MIN) * positions) /
                            (POT_MAX - POT_MIN + 1));
}

int AppTemplate::ChasePhrasePosition(int current, int target)
{
    const int difference = target - current;
    if (difference == 0) return current;
    const int amount = std::min(3, std::max(1, (std::abs(difference) + 1) / 2));
    return current + (difference > 0 ? amount : -amount);
}

void AppTemplate::SamplePatchTargetsForPhrase()
{
    // STEP is deliberately held for the whole phrase; FREE remains continuous.
    if (step_patch_.active) step_atmos_value_ = step_patch_.filtered_value;

    const int tension_target = requested_tension_;
    tension_chased_ = std::clamp(ChasePhrasePosition(tension_chased_, tension_target),
                                  0, kTensionPositions - 1);
    tension_amount_ = tension_chased_;
    adopted_tension_ = tension_amount_;
    effective_tension_ = adopted_tension_;
    if (tension_amount_ <= 0)
    {
        tension_phase_ = TensionPhase::STABLE;
        tension_hold_phrases_ = 0;
    }

    // Physical WANDER parks on the seven named even destinations, while
    // patch/CV retains the full thirteen-position musical resolution.
    const int wander_target = (wander_patch_.active || wander_param3_patch_.active) ?
        MapControlToPosition(wander_cv_value_, kMaxDistance + 1) :
        distance_pot_->GetMappedValue();
    wander_chased_ = std::clamp(wander_target, 0, kMaxDistance);
    effective_wander_ = wander_chased_;
    if (wander_chased_ != requested_distance_)
    {
        requested_distance_ = wander_chased_;
        ResetLivingState();
    }
}

void AppTemplate::UpdateTensionForPhrase()
{
    if (current_distance_ <= 0 || tension_amount_ <= 0 || preview_mode_ ||
        ending_requested_ || ending_active_ || ending_cadence_active_ || ended_ ||
        shift_audition_ || shift_home_pending_ || shift_release_pending_)
    {
        tension_phase_ = TensionPhase::STABLE;
        tension_hold_phrases_ = 0;
        ClearFlourishState();
        return;
    }

    switch (tension_phase_)
    {
        case TensionPhase::STABLE:
        {
            // Reuse the existing hold counter as a negative
            // cooldown after resolution. No new state is added.
            if (tension_hold_phrases_ < 0)
            {
                tension_hold_phrases_++;
                break;
            }

            // TENSION grants permission for drama; it does not command
            // constant drama. Even at high settings, most phrases stay
            // stable.
            int start_chance = 2 + (tension_amount_ * 2);

            if (current_distance_ >= 7)
                start_chance += 2;
            else if (current_distance_ >= 4)
                start_chance += 1;

            if ((std::rand() % 100) < start_chance)
            {
                tension_phase_ = TensionPhase::BUILD;
                flourish_candidate_ = false;
                // Choose only from existing arcs.  A candidate has no effect
                // unless this BUILD actually reaches HOLD.
                if (tension_amount_ == 8)
                    flourish_candidate_ = (std::rand() % 100) < 10;
                else if (tension_amount_ == 9)
                    flourish_candidate_ = (std::rand() % 100) < 20;
                else if (tension_amount_ == 10)
                    flourish_candidate_ = (std::rand() % 100) < 33;
                if (flourish_candidate_ && tension_amount_ >= 9)
                    flourish_ = tension_amount_ == 10 ? Flourish::FULL :
                                                        Flourish::LEAD_IN;
            }
            break;
        }

        case TensionPhase::BUILD:
        {
            // Most tension gestures are brief. Higher settings make a
            // sustained HOLD more likely, but never inevitable.
            int hold_chance = 20 + (tension_amount_ * 7);

            if ((std::rand() % 100) < hold_chance)
            {
                tension_phase_ = TensionPhase::HOLD;
                if (flourish_candidate_)
                {
                    flourish_ = tension_amount_ == 10 ? Flourish::FULL :
                                                        Flourish::CONSERVATIVE;
                    flourish_rub_pending_ = true;
                    flourish_open_hat_pending_ = tension_amount_ == 10;
                }

                tension_hold_phrases_ =
                    (tension_amount_ >= 6 &&
                     (std::rand() % 100) < 18)
                        ? 2
                        : 1;
            }
            else
            {
                tension_phase_ = TensionPhase::RESOLVE;
                ClearFlourishState();
            }
            break;
        }

        case TensionPhase::HOLD:
            if (tension_hold_phrases_ > 1)
            {
                tension_hold_phrases_--;
            }
            else
            {
                tension_hold_phrases_ = 0;
                tension_phase_ = TensionPhase::RESOLVE;
            }
            break;

        case TensionPhase::RESOLVE:
            tension_phase_ = TensionPhase::STABLE;
            ClearFlourishState();

            // Four to seven whole phrases of guaranteed stability.
            // Negative values distinguish cooldown from HOLD without
            // adding another state variable.
            tension_hold_phrases_ =
                -(4 + (std::rand() % 4));
            break;
    }
}


// ------------------------------------------------------------
// ATMOSPHERE
// ------------------------------------------------------------
//
// This fox has a different job: hold harmonic space across phrases.
// HOME anchors the root. WANDER permits stable scale-colour tones and
// occasional empty space. RETURN progressively narrows back HOME.

int AppTemplate::ChooseAtmosphereDegree() const
{
    int roll = std::rand() % 100;
    const int root = ClampPitchDegree(home_pitch_[0]);
    const auto home_interval = [this, root](int interval) {
        return ClampPitchDegree(root + interval);
    };

    if (current_distance_ <= 3)
    {
        if (roll < 75) return root;
        if (roll < 92) return home_interval(4);  // fifth
        return home_interval(2);                 // third
    }

    if (current_distance_ <= 7)
    {
        if (roll < 38) return root;
        if (roll < 66) return home_interval(2);
        if (roll < 90) return home_interval(4);
        return home_interval(7);                 // octave
    }

    if (roll < 22) return root;
    if (roll < 48) return home_interval(2);
    if (roll < 78) return home_interval(4);
    return home_interval(7);
}


void AppTemplate::UpdateAtmosphereForPhrase()
{
    // Atmosphere makes the tension arc physically audible underneath
    // the melodic voices. BUILD suspends on the fourth; HOLD uses a
    // restrained tritone above the root; RESOLVE lands firmly on root.
    const int root = ClampPitchDegree(home_pitch_[0]);
    if (tension_phase_ == TensionPhase::BUILD)
    {
        atmosphere_degree_ = ClampPitchDegree(root + 3);
        atmosphere_active_ = true;
        atmosphere_osc_.SetFrequency(
            ScaleDegreeToFrequency(atmosphere_degree_));
        atmosphere_env_.Trigger();
        return;
    }

    if (tension_phase_ == TensionPhase::HOLD)
    {
        atmosphere_active_ = true;

        // Most HOLDs stay suspended on the fourth. The tritone is saved
        // for occasional stronger moments, especially at high TENSION.
        int tritone_chance =
            8 + (tension_amount_ * 5);

        if (flourish_ != Flourish::NONE || (std::rand() % 100) < tritone_chance)
        {
            atmosphere_osc_.SetFrequency(
                ChromaticOffsetFrequency(root, 6));
        }
        else
        {
            atmosphere_degree_ = ClampPitchDegree(root + 3);
            atmosphere_osc_.SetFrequency(
                ScaleDegreeToFrequency(atmosphere_degree_));
        }

        atmosphere_env_.Trigger();
        return;
    }

    if (tension_phase_ == TensionPhase::RESOLVE)
    {
        atmosphere_degree_ = root;
        atmosphere_active_ = true;
        atmosphere_osc_.SetFrequency(
            ScaleDegreeToFrequency(root));
        atmosphere_env_.Trigger();
        return;
    }

    if (current_distance_ <= 0)
    {
        atmosphere_degree_ = root;
        atmosphere_active_ = true;
    }
    else if (requested_distance_ < current_distance_)
    {
        // A fifth is a stable signpost while still far away; near HOME,
        // the atmosphere resolves all the way to the root.
        atmosphere_degree_ =
            (current_distance_ <= 4) ? root : ClampPitchDegree(root + 4);

        atmosphere_active_ = true;
    }
    else
    {
        int active_chance = 70;

        if (current_distance_ >= 8)
            active_chance = 88;
        else if (current_distance_ >= 4)
            active_chance = 80;

        atmosphere_active_ =
            (std::rand() % 100) < active_chance;

        if (!atmosphere_active_)
        {
            // No hard mute: let the existing long envelope breathe out.
            return;
        }

        atmosphere_degree_ =
            ChooseAtmosphereDegree();
    }

    atmosphere_osc_.SetFrequency(
        ScaleDegreeToFrequency(
            atmosphere_degree_));

    atmosphere_env_.Trigger();
}


// ------------------------------------------------------------
// PERCUSSION
// ------------------------------------------------------------
//
// This fox is composition-aware rather than clock-random.
//
// HOME:
//   Step 0 = strong phrase anchor.
//   Step 4 = restrained midpoint anchor.
//
// WANDER:
//   The anchors remain recognizable, but each phrase may earn a
//   short fill at its end.  Greater distance increases both the
//   chance and the maximum length of that fill.
//
// Crucially, distance is PERMISSION, not a density command.
// Even in the wilderness some phrases deliberately get no fill.

void AppTemplate::PlanPercussionPhrase()
{
    if (ending_cadence_active_)
    {
        // Dedicated ending bars are authored independently of HOME.
        percussion_groove_changed_ = true;
        percussion_fill_length_ = 0;
        const EndingPlan &plan = kEndingPlans[ending_plan_];
        percussion_pattern_mask_ = plan.kick[ending_cadence_bar_];
        snare_pattern_mask_ = plan.snare[ending_cadence_bar_];
        hihat_pattern_mask_ = 0;
        hihat_fill_style_ = 0;
        hihat_subhit_1_samples_ = -1;
        hihat_subhit_2_samples_ = -1;
        hihat_subhit_3_samples_ = -1;
        return;
    }

    if (percussion_groove_phrases_ > 0)
    {
        --percussion_groove_phrases_;
        percussion_groove_changed_ = false;
        percussion_fill_length_ = 0;
        return;
    }

    percussion_groove_changed_ = true;
    percussion_pattern_mask_ = 0;
    snare_pattern_mask_ = 0;
    percussion_fill_length_ = 0;
    int home_attacks = 0;
    for (size_t step = 0; step < kPhraseLength; ++step)
        if (home_rhythm_[step]) ++home_attacks;

    // The downbeat is always structural; other kick accents only reinforce
    // existing HOME attacks and get sparser as the song gets denser.
    percussion_pattern_mask_ |= 1u;
    const int accent_chance = 20 + tension_amount_ * 6 - home_attacks * 3;
    for (size_t step = 1; step < kPhraseLength - 1; ++step)
        if (home_rhythm_[step] && (std::rand() % 100) < accent_chance)
            percussion_pattern_mask_ |= static_cast<uint8_t>(1u << step);

    // Punctuation follows a structural attack with an empty response space.
    for (size_t step = 1; step < kPhraseLength - 1; ++step)
        if (!home_rhythm_[step] && home_rhythm_[step - 1] &&
            (std::rand() % 100) < 18 + tension_amount_ * 4)
            snare_pattern_mask_ |= static_cast<uint8_t>(1u << step);

    const int fill_chance = 6 + tension_amount_ * 4;
    if ((std::rand() % 100) < fill_chance)
        percussion_fill_length_ = tension_amount_ >= 6 ? 2 : 1;

    if (tension_phase_ == TensionPhase::BUILD &&
        (flourish_ == Flourish::LEAD_IN || flourish_ == Flourish::FULL))
        percussion_fill_length_ = 2;
    if (tension_phase_ == TensionPhase::HOLD && flourish_ != Flourish::NONE)
    {
        // A sparse, repeatable peak punctuation; the normal groove remains.
        percussion_pattern_mask_ |= 1u;
        snare_pattern_mask_ |= static_cast<uint8_t>(1u << 4);
    }

    // Persist a learned groove for several phrases; fills remain phrase-aware.
    percussion_groove_phrases_ = 3 + (std::rand() % 4);
}

void AppTemplate::CancelMyTurn()
{
    my_turn_phase_ = MyTurnPhase::NONE;
    // A cancelled special presentation restarts the already-drawn ordinary
    // interval without consuming another musical random decision.
    my_turn_normal_phrases_ = 0;
}

void AppTemplate::ArmNextMyTurn()
{
    my_turn_normal_phrases_ = 0;
    my_turn_arm_phrases_ = kMyTurnMinArmPhrases +
        (std::rand() % (kMyTurnMaxArmPhrases - kMyTurnMinArmPhrases + 1));
}

void AppTemplate::ResetMyTurnPerformance()
{
    CancelMyTurn();
    ArmNextMyTurn();
}


void AppTemplate::TriggerPercussionForStep(
    size_t step)
{
    // Existing ending plans 5, 6, and 7 supply this four-phrase solo arc:
    // statement, answer, peak, then a decisive finish.  The hats and fills
    // below reuse the existing drum voices and final-interval scheduler.
    // MY TURN only borrows this presentation; normal planners still run.
    static constexpr uint8_t kMyTurnKick[] = {0x11, 0x25, 0x2b, 0xd5};
    static constexpr uint8_t kMyTurnSnare[] = {0x40, 0x48, 0x94, 0xf0};
    static constexpr uint8_t kMyTurnHat[] = {0x55, 0x77, 0xff, 0x55};
    static constexpr uint8_t kMyTurnFillLength[] = {0, 1, 3, 2};
    static constexpr uint8_t kMyTurnHatSubdivision[] = {0, 1, 3, 4};
    const bool my_turn_active = my_turn_phase_ != MyTurnPhase::NONE;
    const size_t my_turn_bar = my_turn_active ?
        static_cast<size_t>(my_turn_phase_) - 1 : 0;
    const uint8_t kick_mask = my_turn_active ? kMyTurnKick[my_turn_bar] :
        percussion_pattern_mask_;
    const uint8_t snare_mask = my_turn_active ? kMyTurnSnare[my_turn_bar] :
        snare_pattern_mask_;
    const uint8_t fill_length = my_turn_active ?
        kMyTurnFillLength[my_turn_bar] : percussion_fill_length_;
    bool hit = false;

    hit = (kick_mask & static_cast<uint8_t>(1u << step)) != 0;

    // Phrase-ending fill vocabulary v1:
    // length 1 -> step 7
    // length 2 -> steps 6,7
    // length 3 -> steps 5,6,7
    //
    // This first version intentionally stays on the master grid.
    // Later foxes can add subdivisions/accents once the musical
    // behaviour has proved itself by ear.
    if (fill_length > 0)
    {
        size_t fill_start =
            kPhraseLength -
            static_cast<size_t>(
                fill_length);

        if (step >= fill_start)
        {
            hit = true;
        }
    }

    if (hit)
    {
        // A tiny pitch distinction makes fill hits read as activity
        // without requiring a second percussion voice.
        if (fill_length > 0 &&
            step >=
                kPhraseLength -
                static_cast<size_t>(
                    fill_length))
        {
            percussion_osc_.SetFrequency(
                (step == kPhraseLength - 1)
                    ? 96.0f
                    : 118.0f);
        }
        else if (ending_active_ && step == kPhraseLength - 1)
        {
            percussion_osc_.SetFrequency(72.0f);
        }
        else
        {
            percussion_osc_.SetFrequency(
                82.0f);
        }

        percussion_env_.Trigger();
    }

    if (!shift_audition_ &&
        (snare_mask & static_cast<uint8_t>(1u << step)))
        snare_env_.Trigger();

    if (my_turn_active)
    {
        if (kMyTurnHat[my_turn_bar] & static_cast<uint8_t>(1u << step))
            TriggerHiHat(false);

        if (step == kPhraseLength - 1 && last_clock_interval_samples_ > 0)
        {
            switch (kMyTurnHatSubdivision[my_turn_bar])
            {
            case 1: // Existing half-interval closed-hat fill.
                hihat_subhit_1_samples_ = last_clock_interval_samples_ / 2;
                break;
            case 3: // Existing quarter/half/three-quarter hat fill.
                hihat_subhit_1_samples_ = last_clock_interval_samples_ / 4;
                hihat_subhit_2_samples_ = last_clock_interval_samples_ / 2;
                hihat_subhit_3_samples_ =
                    (last_clock_interval_samples_ * 3) / 4;
                break;
            case 4: // Existing open-hat half-interval transition gesture.
                hihat_subhit_1_samples_ = last_clock_interval_samples_ / 2;
                hihat_subhit_1_open_ = true;
                break;
            default:
                break;
            }
        }
    }
}

void AppTemplate::ClearFlourishState()
{
    flourish_ = Flourish::NONE;
    flourish_candidate_ = false;
    flourish_rub_pending_ = false;
    flourish_open_hat_pending_ = false;
}

void AppTemplate::SelectEndingPlan()
{
    ending_tension_ = static_cast<uint8_t>(std::clamp(tension_amount_, 0, 10));
    uint8_t first = 0;
    uint8_t count = 5;
    const int roll = std::rand() % 100;
    if (ending_tension_ == 10 && roll < 12)
    {
        first = kSecretEndingPlan;
        count = 1;
    }
    else if ((ending_tension_ == 8 && roll < 12) ||
             (ending_tension_ == 9 && roll < 22) ||
             (ending_tension_ == 10 && roll < 52))
    {
        first = 5;
        count = 2;
    }
    ending_plan_ = static_cast<uint8_t>(first + (std::rand() % count));
    if (count > 1 && ending_plan_ == previous_ending_plan_)
        ending_plan_ = static_cast<uint8_t>(first + ((ending_plan_ - first + 1) % count));
    // The secret is never selected twice in a row, even if a normal ending
    // intervenes only through a restart.
    if (ending_plan_ == kSecretEndingPlan && previous_ending_plan_ == kSecretEndingPlan)
        ending_plan_ = static_cast<uint8_t>(std::rand() % 5);
    ending_cadence_bars_ = kEndingPlans[ending_plan_].bars;
    ending_cadence_bar_ = 0;
    previous_ending_plan_ = ending_plan_;
}


void AppTemplate::SetVoiceAOutputLevel(VoiceCharacter character)
{
    const auto &definition = kVoicePalette[static_cast<size_t>(character)];
    q15_t level = static_cast<q15_t>(definition.output_level * Q15_MAX);
    if (character == VoiceCharacter::SYNTH)
        // 0.669 * 0.90 * 0.85 * 0.90, consolidated after Q15 rounding.
        level = 13582;
    else if (character == VoiceCharacter::REED)
        level = q15_mult(level, q15(0.95f));
    voice_a_output_level_ = level;
}

void AppTemplate::MutePercussionOrnaments()
{
    // SHIFT is an arrangement mute: retain the current kick phrase and its
    // phase, but remove any already-decaying snare/hat ornament immediately.
    hihat_subhit_1_samples_ = -1;
    hihat_subhit_2_samples_ = -1;
    hihat_subhit_3_samples_ = -1;
    hihat_subhit_1_open_ = false;
    transition_open_hat_pending_ = false;
    hihat_env_.Reset();
    snare_env_.Reset();
}

void AppTemplate::PlanHiHatPhrase()
{
    if (ending_cadence_active_)
    {
        // Let the kick/snare cadence own the final beat.
        hihat_pattern_mask_ = 0;
        hihat_fill_style_ = 0;
        return;
    }

    if (hihat_phrase_replans_ > 0)
    {
        --hihat_phrase_replans_;
        return;
    }

    hihat_pattern_mask_ = 0;
    hihat_fill_style_ = 0;
    hihat_phrase_replans_ = std::rand() & 1;
    // At low tension, an occasional entirely quiet closed-hat phrase leaves
    // room for the motif. Transition open hats are planned independently.
    if (tension_amount_ <= 3 && (std::rand() % 100) < 20)
        return;

    int home_attacks = 0;
    for (size_t step = 0; step < kPhraseLength; ++step)
        if (home_rhythm_[step]) ++home_attacks;
    const int response_chance = 14 + tension_amount_ * 3 +
        (8 - home_attacks) * 4;
    const int home_attack_chance = 70 + tension_amount_;
    for (size_t step = 0; step < kPhraseLength; ++step)
    {
        const bool home_attack = home_rhythm_[step];
        const bool eligible_response =
            !(snare_pattern_mask_ & static_cast<uint8_t>(1u << step));
        if (step == 0 ||
            (home_attack ? (std::rand() % 100) < home_attack_chance :
                           (eligible_response &&
                            (std::rand() % 100) < response_chance)))
            hihat_pattern_mask_ |= static_cast<uint8_t>(1u << step);
    }
    if (percussion_fill_length_ > 0 && tension_amount_ >= 3)
        hihat_fill_style_ = tension_amount_ >= 7 ? 2 : 1;
}

void AppTemplate::PlanTransitionPercussionPhrase()
{
    drum_transition_ = DrumTransition::NONE;
    transition_open_hat_pending_ = false;

    // Preview, SHIFT's HOME audition, and the ending/restart path retain
    // their own isolated arrangements and never borrow transition ornaments.
    if (preview_mode_ || ended_ || ending_requested_ || ending_active_ ||
        ending_cadence_active_ || shift_audition_ || shift_home_pending_ ||
        shift_release_pending_)
    {
        return;
    }

    if (current_distance_ < requested_distance_)
        drum_transition_ = DrumTransition::OUTWARD;
    else if (current_distance_ > requested_distance_)
        drum_transition_ = requested_distance_ == 0 ?
            DrumTransition::HOME : DrumTransition::INWARD;
    else if (current_distance_ > 0 && living_initialized_ &&
             living_hold_phrases_ == 0)
    {
        // The living engine will either develop the local idea or return from
        // one variation at the next boundary; its musical result stays there.
        drum_transition_ = DrumTransition::LIVING_DEVELOPMENT;
    }

    int response_chance = 0;
    switch (drum_transition_)
    {
    case DrumTransition::HOME:
        response_chance = 100;
        break;
    case DrumTransition::INWARD:
        if (tension_amount_ >= 3) response_chance = 45 + tension_amount_ * 5;
        break;
    case DrumTransition::OUTWARD:
        if (tension_amount_ >= 4) response_chance = 35 + tension_amount_ * 6;
        break;
    case DrumTransition::LIVING_DEVELOPMENT:
        if (tension_amount_ >= 6)
            response_chance = 25 + (tension_amount_ - 6) * 12;
        break;
    case DrumTransition::NONE:
        break;
    }

    transition_open_hat_pending_ = response_chance > 0 &&
        (response_chance >= 100 || (std::rand() % 100) < response_chance);
}

void AppTemplate::UpdateCymbalRestraintForPhrase()
{
    const bool eligible = !ending_requested_ && !ending_active_ &&
        !ending_cadence_active_ && !preview_mode_ && !shift_audition_ &&
        !shift_home_pending_ && !shift_release_pending_ &&
        my_turn_phase_ == MyTurnPhase::NONE;
    if (!eligible)
    {
        // Special presentations own their cymbal language. They pause a
        // pending interval, while an active restraint is cleanly cancelled.
        if (cymbal_restraint_phrases_remaining_ > 0)
        {
            cymbal_restraint_phrases_remaining_ = 0;
            cymbal_restraint_normal_phrases_ = 0;
        }
        return;
    }

    if (cymbal_restraint_phrases_remaining_ > 0)
    {
        --cymbal_restraint_phrases_remaining_;
        if (cymbal_restraint_phrases_remaining_ == 0)
        {
            ArmNextCymbalRestraint();
            // This boundary starts the first ordinary phrase after restraint.
            cymbal_restraint_normal_phrases_ = 1;
        }
        return;
    }

    if (cymbal_restraint_normal_phrases_ < cymbal_restraint_arm_phrases_)
    {
        ++cymbal_restraint_normal_phrases_;
        return;
    }

    // The normal interval is already complete; keep the approved duration.
    cymbal_restraint_phrases_remaining_ = 1 + (std::rand() & 1);
}

void AppTemplate::ArmNextCymbalRestraint()
{
    cymbal_restraint_normal_phrases_ = 0;
    cymbal_restraint_arm_phrases_ = kCymbalRestraintMinArmPhrases +
        (std::rand() % (kCymbalRestraintMaxArmPhrases -
                        kCymbalRestraintMinArmPhrases + 1));
}

bool AppTemplate::IsCymbalRestraintActive() const
{
    return cymbal_restraint_phrases_remaining_ > 0;
}

void AppTemplate::ScheduleHiHatSubStepFill(
    size_t step)
{
    // Compact fills occur only after the final phrase step.  They occupy
    // the space BEFORE the next master-clock event instead of consuming
    // earlier full phrase steps.
    if (step != kPhraseLength - 1 ||
        last_clock_interval_samples_ <= 0)
    {
        return;
    }

    hihat_subhit_1_samples_ = -1;
    hihat_subhit_2_samples_ = -1;
    hihat_subhit_3_samples_ = -1;
    hihat_subhit_1_open_ = false;

    // A confirmed structural change gets one anticipatory off-beat.  This
    // reuses the measured-clock scheduler; the normal downbeat kick marks
    // the arrival without adding another percussion voice.
    if ((transition_open_hat_pending_ || flourish_open_hat_pending_) && !ending_requested_ && !ending_active_ &&
        !ending_cadence_active_ && !ended_ && !preview_mode_ &&
        !shift_audition_ && !shift_home_pending_ && !shift_release_pending_)
    {
        hihat_subhit_1_samples_ = last_clock_interval_samples_ / 2;
        hihat_subhit_1_open_ = true;
        flourish_open_hat_pending_ = false;
        return;
    }

    if (IsCymbalRestraintActive())
        return;

    if (hihat_fill_style_ <= 0)
        return;

    if (hihat_fill_style_ == 1)
    {
        hihat_subhit_1_samples_ =
            last_clock_interval_samples_ / 2;
    }
    else if (hihat_fill_style_ == 2)
    {
        hihat_subhit_1_samples_ =
            last_clock_interval_samples_ / 3;

        hihat_subhit_2_samples_ =
            (last_clock_interval_samples_ * 2) / 3;
    }
    else
    {
        hihat_subhit_1_samples_ =
            last_clock_interval_samples_ / 4;

        hihat_subhit_2_samples_ =
            last_clock_interval_samples_ / 2;

        hihat_subhit_3_samples_ =
            (last_clock_interval_samples_ * 3) / 4;
    }
}


void AppTemplate::ProcessHiHatSubStepScheduler()
{
    auto process_one =
        [this](int &samples_remaining, bool open)
        {
            if (samples_remaining < 0)
            {
                return;
            }

            if (samples_remaining == 0)
            {
                TriggerHiHat(open);
                samples_remaining = -1;
                return;
            }

            samples_remaining--;
        };

    process_one(hihat_subhit_1_samples_, hihat_subhit_1_open_);
    process_one(hihat_subhit_2_samples_, false);
    process_one(hihat_subhit_3_samples_, false);
}

void AppTemplate::InvalidateClockOwnedDelayedEvents()
{
    // These countdowns belong to the clock interval that scheduled them.
    // They must never cross a later master edge or clock-context change.
    hihat_subhit_1_samples_ = -1;
    hihat_subhit_2_samples_ = -1;
    hihat_subhit_3_samples_ = -1;
    hihat_subhit_1_open_ = false;

    // Preview timing is likewise confined to its source interval. Unlike
    // the ordinary phrase scheduler, Preview has no intentional pickup.
    preview_pending_note_ = false;
    preview_pending_delay_samples_ = 0;
}

void AppTemplate::TriggerHiHat(bool open)
{
    // Selecting the decay immediately before Trigger preserves the original
    // closed tick and lets every later hat hit naturally choke an open one.
    hihat_env_.SetDecayTime(open ? 0.085f : 0.024f);
    hihat_env_.Trigger();
}

void AppTemplate::TriggerHiHatForStep(
    size_t step)
{
    bool hit =
        (hihat_pattern_mask_ &
         static_cast<uint8_t>(1u << step)) != 0;

    if (hit && !IsCymbalRestraintActive())
    {
        TriggerHiHat(false);
    }

    ScheduleHiHatSubStepFill(step);
}



// ------------------------------------------------------------
// LIVING DISTANCE RESET
// ------------------------------------------------------------

void AppTemplate::ResetLivingState()
{
    living_initialized_ = false;
    living_variation_active_ = false;
    consecutive_trivial_variations_ = 0;
    living_hold_phrases_ = 0;
}

void AppTemplate::ResetVoiceARefrainState()
{
    home_refrain_active_ = false;
    home_refrain_phrases_remaining_ = 0;
    ClearStructuredHomePresentation();
    normal_refrains_since_home_ = 0;
    home_refrain_defer_ = 0;
    voice_a_refrain_signature_ = 0;
    voice_a_refrain_repeats_ = 0;
}

uint32_t AppTemplate::VoiceARefrainSignature(bool home_plan) const
{
    uint32_t signature = 2166136261u;
    const auto &rhythm = home_plan ? home_rhythm_ : current_rhythm_;
    const auto &pitch = home_plan ? home_pitch_ : current_pitch_;
    const auto &timing = home_plan ? home_timing_samples_ : current_timing_samples_;
    const auto mix = [&signature](uint32_t value)
    {
        signature ^= value;
        signature *= 16777619u;
    };
    mix(static_cast<uint32_t>(selected_voice_a_));
    // HOME's family can supply the step-7 pickup; WANDER distance itself is
    // deliberately omitted because the ceiling is about audible Voice A.
    mix(static_cast<uint32_t>(home_plan ? home_family_ : 0));
    for (size_t step = 0; step < kPhraseLength; ++step)
    {
        mix(rhythm[step] ? 1u : 0u);
        if (rhythm[step])
        {
            mix(static_cast<uint32_t>(pitch[step] + 32));
            mix(static_cast<uint32_t>(timing[step]));
        }
    }
    return signature;
}

void AppTemplate::ForceAudibleVoiceAChange()
{
    // The existing living grammar has already had its turn. This only
    // prevents a fifth exact repeat when it produced the same presentation.
    for (size_t step = 1; step < kPhraseLength; ++step)
    {
        if (current_rhythm_[step])
        {
            current_pitch_[step] += current_pitch_[step] < kHomePitchMaximum ? 1 : -1;
            return;
        }
    }
    current_rhythm_[1] = true;
}

void AppTemplate::UpdateVoiceARefrainPresentation()
{
    if (voice_a_lab_off_ || current_distance_ <= 0)
    {
        voice_a_refrain_signature_ = 0;
        voice_a_refrain_repeats_ = 0;
        return;
    }

    const bool home_plan = home_refrain_active_;
    uint32_t signature = VoiceARefrainSignature(home_plan);
    const uint8_t repeats = signature == voice_a_refrain_signature_ ?
        static_cast<uint8_t>(voice_a_refrain_repeats_ + 1) : 1;

    if (home_plan && repeats > 4)
    {
        // A quotation never extends a five-repeat presentation streak.
        home_refrain_active_ = false;
        home_refrain_phrases_remaining_ = 0;
        ClearStructuredHomePresentation();
        normal_refrains_since_home_ = 0;
        ForceAudibleVoiceAChange();
        signature = VoiceARefrainSignature(false);
        voice_a_refrain_repeats_ = 1;
    }
    else if (!home_plan && repeats > 4)
    {
        ForceAudibleVoiceAChange();
        signature = VoiceARefrainSignature(false);
        voice_a_refrain_repeats_ = 1;
    }
    else
    {
        voice_a_refrain_repeats_ = repeats;
    }
    voice_a_refrain_signature_ = signature;
}


// ------------------------------------------------------------
// EXACT HOME
// ------------------------------------------------------------

void AppTemplate::RestoreHome()
{
    current_rhythm_ =
        home_rhythm_;

    current_pitch_ =
        home_pitch_;

    current_timing_samples_ =
        home_timing_samples_;

    current_distance_ = 0;

    ResetLivingState();
    ResetVoiceARefrainState();
}


// ------------------------------------------------------------
// HOME BUILDER — RANGE
// ------------------------------------------------------------

// HOME COMPOSER
// ------------------------------------------------------------

void AppTemplate::GenerateHomeRhythm(bool signature)
{
    GenerateRhythmFamily(rhythm_, home_rhythm_, home_timing_samples_, signature);
}

void AppTemplate::GenerateRhythmFamily(
    int family,
    std::array<bool, kPhraseLength> &rhythm,
    std::array<int, kPhraseLength> &timing,
    bool signature,
    bool require_different)
{
    // Curated family-safe masks. Signature generation retains the canonical
    // first mask; ordinary generation chooses freely from the full bank.
    static constexpr uint8_t patterns[8][5] = {
        {0x19, 0x1D, 0x29, 0x00, 0x00}, // Sparse: X..XX..., X.XXX..., X..X.X..
        {0x55, 0x57, 0x5D, 0xD5, 0x00}, // Steady
        {0x33, 0x13, 0x31, 0x53, 0x00}, // Call and Answer
        {0x49, 0x4B, 0x59, 0xC9, 0x00}, // 3+3+2
        {0xFF, 0xFD, 0xF7, 0xDF, 0x7F}, // Swing
        {0x95, 0x97, 0x9D, 0xB5, 0xD5}, // Pickup
        {0x5B, 0x4B, 0xDB, 0xCB, 0x00}, // Broken
        {0xFF, 0xFD, 0xF7, 0xDF, 0x7F}  // Urgent
    };
    static constexpr uint8_t pattern_counts[8] = {3, 4, 4, 4, 5, 5, 4, 5};

    uint8_t current_mask = 0;
    for (size_t step = 0; step < kPhraseLength; ++step)
        if (rhythm[step]) current_mask |= static_cast<uint8_t>(1u << step);

    const uint8_t count = pattern_counts[family];
    uint8_t selected = 0;
    if (!signature)
    {
        int current_index = -1;
        if (require_different)
        {
            for (uint8_t index = 0; index < count; ++index)
                if (patterns[family][index] == current_mask)
                    current_index = index;
        }
        if (current_index >= 0)
        {
            selected = static_cast<uint8_t>(std::rand() % (count - 1));
            if (selected >= current_index) ++selected;
        }
        else
        {
            selected = static_cast<uint8_t>(std::rand() % count);
        }
    }

    const uint8_t mask = patterns[family][selected];
    for (size_t i = 0; i < kPhraseLength; ++i)
    {
        rhythm[i] = (mask & static_cast<uint8_t>(1u << i)) != 0;
        timing[i] = 0;
    }
}

void AppTemplate::GenerateHomePitch()
{
    const int lo = std::min(low_, high_);
    const int hi = std::max(low_, high_);
    const int span = hi - lo;
    const int root = lo + std::rand() % (span + 1);
    const auto bound = [lo, hi](int degree) {
        return std::max(lo, std::min(hi, degree));
    };

    // A is a small contour-led gesture within the quantized LOW/HIGH
    // sandbox. The first move also gives families with few attacks a
    // recognizable melodic direction.
    const int direction = (std::rand() & 1) ? 1 : -1;
    home_pitch_[0] = root;
    home_pitch_[1] = bound(root + direction);
    if (home_pitch_[1] == root) home_pitch_[1] = bound(root - direction);
    const int turn = (std::rand() % 3 == 0) ? -direction : direction;
    home_pitch_[2] = bound(home_pitch_[1] + turn);
    home_pitch_[3] = bound(home_pitch_[2] +
        ((std::rand() % 4 == 0) ? 2 * turn : -turn));

    // A' quotes the opening, with one restrained response change.
    home_pitch_[4] = home_pitch_[0];
    home_pitch_[5] = home_pitch_[1];
    home_pitch_[6] = home_pitch_[2];
    if (home_rhythm_[6] && rhythm_ != 3 && (std::rand() & 1))
        home_pitch_[6] = bound(home_pitch_[6] - turn);

    // Each family retains its own last sounding position. Its cadence
    // returns to the opening tonal centre, including Sparse's step 4
    // and Pickup's anticipated step 7.
    size_t final_attack = 0;
    for (size_t i = 0; i < kPhraseLength; ++i)
        if (home_rhythm_[i]) final_attack = i;
    home_pitch_[7] = home_pitch_[6];
    home_pitch_[final_attack] = root;

    std::array<size_t, kPhraseLength> attacks{};
    size_t attack_count = 0;
    for (size_t i = 0; i < kPhraseLength; ++i)
        if (home_rhythm_[i]) attacks[attack_count++] = i;

    const size_t low_attack = attacks[std::rand() % attack_count];
    home_pitch_[low_attack] = lo;
    if (lo != hi)
    {
        size_t high_attack = low_attack;
        while (high_attack == low_attack)
            high_attack = attacks[std::rand() % attack_count];
        home_pitch_[high_attack] = hi;
    }
}

int AppTemplate::HomeStepDelay(size_t step) const
{
    return RhythmFamilyStepDelay(home_family_, step);
}

int AppTemplate::RhythmFamilyStepDelay(int family, size_t step) const
{
    const int interval = last_clock_interval_samples_;
    if (interval <= MillisecondsToSamples(12)) return 0;
    int delay = 0;
    if (family == 4 && (step & 1)) delay = interval / 3;
    if (family == 5 && step == 7) delay = interval / 3;
    if (family == 6 && (step == 1 || step == 3 || step == 6))
        delay = interval / 4;
    return std::max(0, std::min(delay, interval - MillisecondsToSamples(8)));
}

void AppTemplate::StopAuditionNotes()
{
    ClearFlourishState();
    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
    preview_pending_note_ = false;
    preview_pending_delay_samples_ = 0;
    voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
    voice_b_hold_phrases_remaining_ = 0;
    voice_b_has_harmony_note_ = false;
    voice_b_hold_eligible_ = false;
    pending_note_ = false;
    pending_note_is_resolve_ = false;
    pending_note_delay_samples_ = 0;
    pending_pickup_ = false;
    pickup_recovery_samples_ = 0;
    pending_harmony_ = false;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;
    env_.SetHold(false);
    hihat_subhit_1_samples_ = -1;
    hihat_subhit_2_samples_ = -1;
    hihat_subhit_3_samples_ = -1;
    env_.Reset();
    harmony_env_.Reset();
    percussion_env_.Reset();
    hihat_env_.Reset();
}

void AppTemplate::StopMelodyAuditionNotes()
{
    cadence_melody_release_samples_ = 0;
    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
    preview_pending_note_ = false;
    preview_pending_delay_samples_ = 0;
    voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
    voice_b_hold_phrases_remaining_ = 0;
    voice_b_has_harmony_note_ = false;
    voice_b_hold_eligible_ = false;
    pending_note_ = false;
    pending_note_is_resolve_ = false;
    pending_note_delay_samples_ = 0;
    pending_pickup_ = false;
    pickup_recovery_samples_ = 0;
    pending_harmony_ = false;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;
    reference_audition_ = false;
    env_.SetHold(false);
    env_.Reset();
    harmony_env_.Reset();
    voice_a_oddity_glint_delay_ = 0;
    voice_a_oddity_glint_remaining_ = 0;
    voice_a_oddity_glint_ramp_ = 0;
    voice_a_oddity_glint_envelope_ = 0;
    voice_b_oddity_glint_delay_ = 0;
    voice_b_oddity_glint_remaining_ = 0;
    voice_b_oddity_glint_ramp_ = 0;
    voice_b_oddity_glint_envelope_ = 0;
}

void AppTemplate::BeginCadenceMelodyRelease()
{
    // Kill future melodic work at the cadence downbeat, but retain the
    // current direct A/B sample stream long enough to fade it click-free.
    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
    pending_note_ = false;
    pending_note_is_resolve_ = false;
    pending_note_delay_samples_ = 0;
    pending_pickup_ = false;
    pickup_recovery_samples_ = 0;
    pending_harmony_ = false;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;
    reference_audition_ = false;
    env_.SetHold(false);

    // A delayed glint would be a new cadence event. An already-active glint
    // remains part of the direct stream and is covered by the common fade.
    if (voice_a_oddity_glint_delay_ > 0)
    {
        voice_a_oddity_glint_delay_ = 0;
        voice_a_oddity_glint_remaining_ = 0;
        voice_a_oddity_glint_ramp_ = 0;
        voice_a_oddity_glint_envelope_ = 0;
    }
    if (voice_b_oddity_glint_delay_ > 0)
    {
        voice_b_oddity_glint_delay_ = 0;
        voice_b_oddity_glint_remaining_ = 0;
        voice_b_oddity_glint_ramp_ = 0;
        voice_b_oddity_glint_envelope_ = 0;
    }

    cadence_melody_release_samples_ = kCadenceMelodyReleaseSamples;
}

void AppTemplate::RestartEndedPlayback()
{
    // END deliberately leaves time-based tails alone. A later restart is a
    // new performance, so discard only stale transient audio state.
    StopAuditionNotes();
    snare_env_.Reset();
    pending_note_delay_samples_ = 0;
    pending_degree_ = 0;
    pending_harmony_degree_ = 0;
    pending_harmony_chromatic_ = false;
    audition_settle_samples_ = 0;
    reference_audition_ = false;
    shift_audition_ = false;
    shift_home_pending_ = false;
    shift_release_pending_ = false;
    suppress_shift_until_release_ = false;
    atmosphere_lfo_phase_ = 0;
    atmosphere_whistle_level_ = Q15_MAX;
    atmosphere_whistle_ending_divider_ = 0;
    atmosphere_delay_pos_ = 0;
    // History validity makes retained storage logically silent without a
    // costly 12,320-sample clear in the restart control path.
    atmosphere_delay_history_ = 0;
    atmosphere_reverb_left_.fill(0);
    atmosphere_reverb_right_.fill(0);
    atmosphere_reverb_left_pos_ = atmosphere_reverb_right_pos_ = 0;
    atmosphere_reverb_damp_left_ = atmosphere_reverb_damp_right_ = 0;
    atmosphere_reverb_field_left_ = atmosphere_reverb_field_right_ = 0;
    atmosphere_fracture_gate_level_ = Q15_MAX;
    atmosphere_fracture_gate_floor_ = Q15_MAX;
    atmosphere_fracture_gate_age_ = 0;
    atmosphere_fracture_gate_cut_start_ = 0;
    atmosphere_fracture_gate_release_ = 0;
    atmosphere_fracture_gate_decrement_ = 0;
    atmosphere_fracture_gate_active_ = false;
    atmosphere_level_ = 0;
    atmosphere_env_.Reset();
    atmosphere_env_value_ = 0;
    atmosphere_target_ = 0;
    atmosphere_active_ = false;
    atmosphere_event_kind_ = 0;
    atmosphere_event_age_ = 0;
    atmosphere_event_samples_ = 0;
    atmosphere_wait_samples_ = 0;
    atmosphere_filter_ = 0;
    dream_a_ready_ = false;
    dream_b_ready_ = false;
    ending_finalizing_ = false;
    ending_led_frozen_ = false;
    ending_led_wander_value_ = 0;
    ending_led_tension_ = 0;
    ending_home_phrases_remaining_ = 0;
    ending_cadence_bar_ = 0;
    ending_cadence_bars_ = 1;
    ResetMyTurnPerformance();
}

void AppTemplate::GenerateNewHome(bool signature)
{
    home_family_ = rhythm_;
    GenerateHomeRhythm(signature);
    GenerateHomePitch();
    last_voice_b_midi_note_ = -1;
    requested_distance_ = 0;
    RestoreHome();
    StopAuditionNotes();
    phrase_step_ = 0;
    ResetMyTurnPerformance();
}

void AppTemplate::GeneratePreviewRhythm(bool require_different)
{
    GenerateRhythmFamily(preview_density_, preview_rhythm_,
        preview_timing_samples_, false, require_different);
}

void AppTemplate::RestartPreviewRhythm(bool require_different)
{
    GeneratePreviewRhythm(require_different);
    EnsurePreviewAnchors();
    ApplyPreviewAnchors();
    preview_pending_note_ = false;
    preview_pending_delay_samples_ = 0;
    preview_loop_ = PreviewLoop::HOME;
    preview_step_ = 0;
}

void AppTemplate::GeneratePreviewHome()
{
    const int lo = std::min(preview_low_, preview_high_);
    const int hi = std::max(preview_low_, preview_high_);
    const int span = hi - lo;
    for (size_t step = 0; step < kPhraseLength; ++step)
    {
        int shape = 0;
        switch (preview_contour_)
        {
        case 0: shape = static_cast<int>(step); break;                 // RISE
        case 1: shape = 7 - static_cast<int>(step); break;             // FALL
        case 2: shape = 7 - std::abs(7 - 2 * static_cast<int>(step)); break; // ARCH
        case 3: shape = std::abs(7 - 2 * static_cast<int>(step)); break;     // VALLEY
        case 4: shape = (step & 1) ? 7 : 0; break;                     // PENDULUM
        case 5: shape = (step == 0 || step == 4) ? 0 :
            ((step == 2 || step == 6) ? 6 : 3); break;             // WAVE
        case 6: shape = static_cast<int>(step / 2) * 2; break;         // STEPS
        default: shape = std::rand() % 8; break;                       // FREE
        }
        preview_pitch_[step] = lo + (span * shape + 3) / 7;
    }

    EnsurePreviewAnchors();
    ApplyPreviewAnchors();
}

void AppTemplate::EnsurePreviewAnchors()
{
    constexpr uint8_t kInvalidPreviewAnchor = 0xFF;
    const auto valid_attack = [this](uint8_t step) {
        return step < kPhraseLength && preview_rhythm_[step];
    };

    std::array<uint8_t, kPhraseLength> attacks{};
    size_t attack_count = 0;
    for (size_t step = 0; step < kPhraseLength; ++step)
        if (preview_rhythm_[step]) attacks[attack_count++] = static_cast<uint8_t>(step);

    if (!valid_attack(preview_low_anchor_step_))
        preview_low_anchor_step_ = attack_count == 0 ? kInvalidPreviewAnchor :
            attacks[std::rand() % attack_count];

    const int lo = std::min(preview_low_, preview_high_);
    const int hi = std::max(preview_low_, preview_high_);
    if (lo == hi || preview_low_anchor_step_ == kInvalidPreviewAnchor)
    {
        preview_high_anchor_step_ = kInvalidPreviewAnchor;
        return;
    }

    if (valid_attack(preview_high_anchor_step_) &&
        preview_high_anchor_step_ != preview_low_anchor_step_)
        return;

    if (attack_count < 2)
    {
        preview_high_anchor_step_ = kInvalidPreviewAnchor;
        return;
    }

    size_t choice = std::rand() % (attack_count - 1);
    for (size_t index = 0; index < attack_count; ++index)
    {
        if (attacks[index] == preview_low_anchor_step_) continue;
        if (choice-- == 0)
        {
            preview_high_anchor_step_ = attacks[index];
            return;
        }
    }
    preview_high_anchor_step_ = kInvalidPreviewAnchor;
}

void AppTemplate::ApplyPreviewAnchors()
{
    constexpr uint8_t kInvalidPreviewAnchor = 0xFF;
    const int lo = std::min(preview_low_, preview_high_);
    const int hi = std::max(preview_low_, preview_high_);
    if (preview_low_anchor_step_ != kInvalidPreviewAnchor &&
        preview_low_anchor_step_ < kPhraseLength &&
        preview_rhythm_[preview_low_anchor_step_])
        preview_pitch_[preview_low_anchor_step_] = lo;
    if (preview_high_anchor_step_ != kInvalidPreviewAnchor &&
        preview_high_anchor_step_ < kPhraseLength &&
        preview_rhythm_[preview_high_anchor_step_])
        preview_pitch_[preview_high_anchor_step_] = hi;
}

void AppTemplate::TriggerPreviewSine(int degree)
{
    active_voice_a_ = VoiceCharacter::SOFT;
    SetVoiceACharacter(active_voice_a_);
    const float frequency = ScaleDegreeToFrequency(ClampPitchDegree(degree));
    osc_.SetNativeFrequency(freq_to_q31(frequency, SAMPLE_RATE));
    env_.Trigger();
}

void AppTemplate::TriggerPreviewClick()
{
    percussion_osc_.SetFrequency(118.0f);
    percussion_env_.Trigger();
}

void AppTemplate::TriggerPreviewDetentTok()
{
    rhythm_detent_body_osc_.SetFrequency(650.0f);
    rhythm_detent_glint_osc_.SetFrequency(1950.0f);
    rhythm_detent_body_remaining_ = static_cast<uint16_t>(
        MillisecondsToSamples(38));
    rhythm_detent_glint_delay_ = static_cast<uint16_t>(
        MillisecondsToSamples(11));
    rhythm_detent_glint_remaining_ = static_cast<uint16_t>(
        MillisecondsToSamples(27));
}

void AppTemplate::EnterPreviewMode()
{
    ClearFlourishState();
    ResetVoiceARefrainState();
    CancelMyTurn();
    preview_mode_ = true;
    preview_loop_ = PreviewLoop::HOME;
    preview_resume_loop_ = PreviewLoop::HOME;
    preview_step_ = 0;
    preview_low_ = low_;
    preview_high_ = high_;
    preview_density_ = static_cast<int>(
        (static_cast<int64_t>(Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_3) -
            POT_MIN) * 8) / (POT_MAX - POT_MIN + 1));
    preview_contour_ = MapControlToPosition(
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_4), 8);
    GeneratePreviewRhythm();
    preview_low_anchor_step_ = 0xFF;
    preview_high_anchor_step_ = 0xFF;
    GeneratePreviewHome();
    preview_saved_requested_distance_ = requested_distance_;
    preview_pot_baseline_ = {
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_2),
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_3),
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_4),
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_6)};
    StopAuditionNotes();
}

void AppTemplate::CancelPreviewMode()
{
    ClearFlourishState();
    ResetVoiceARefrainState();
    CancelMyTurn();
    preview_mode_ = false;
    preview_loop_ = PreviewLoop::NONE;
    preview_resume_loop_ = PreviewLoop::NONE;
    preview_low_anchor_step_ = 0xFF;
    preview_high_anchor_step_ = 0xFF;
    requested_distance_ = preview_saved_requested_distance_;
    reference_audition_ = false;
    StopAuditionNotes();
    preview_led_until_ms_ = 0;
}

void AppTemplate::CommitPreviewMode()
{
    ClearFlourishState();
    ResetMyTurnPerformance();
    if (ended_)
        RestartEndedPlayback();
    low_ = preview_low_;
    high_ = preview_high_;
    home_rhythm_ = preview_rhythm_;
    home_pitch_ = preview_pitch_;
    last_voice_b_midi_note_ = -1;
    home_timing_samples_.fill(0);
    home_family_ = preview_density_;
    requested_distance_ = 0;
    // A committed Preview always returns to normal transport, whether it was
    // entered during playback or from ENDED.
    ended_ = false;
    ending_requested_ = false;
    ending_active_ = false;
    ending_cadence_active_ = false;
    ending_finalizing_ = false;
    atmosphere_whistle_ending_divider_ = 0;
    ending_led_frozen_ = false;
    ending_home_phrases_remaining_ = 0;
    ending_cadence_bar_ = 0;
    ending_cadence_bars_ = 1;
    ending_tension_ = 0;
    RestoreHome();
    // This is a transaction into a fresh transport downbeat. Invalidate every
    // delayed audition/pickup state before firing it, then ignore the still
    // held Preview SHIFT until it has physically been released.
    StopAuditionNotes();
    reference_audition_ = false;
    audition_settle_samples_ = 0;
    shift_audition_ = false;
    shift_home_pending_ = false;
    shift_release_pending_ = false;
    suppress_shift_until_release_ = true;
    preview_mode_ = false;
    preview_loop_ = PreviewLoop::NONE;
    // Commit is a handoff into the new composition, not a silent staging
    // state. Start its guaranteed downbeat now; the next clock advances to
    // step one as usual.
    phrase_step_ = 1;
    tension10_chromatic_lock_ = tension_amount_ == 10 &&
        (std::rand() % 100) < 55;
    SchedulePhraseNote(0);
    TriggerPendingNote();
    preview_led_until_ms_ = to_ms_since_boot(get_absolute_time()) +
                             kPreviewCommitLedMs;
}

void AppTemplate::ReadStartupHomeControls()
{
    // Kastle2 initializes and settles ADC before app/audio startup.
    const auto quantize = [](int32_t value, int positions) {
        if (value < POT_MIN) value = POT_MIN;
        if (value > POT_MAX) value = POT_MAX;
        return static_cast<int>((static_cast<int64_t>(value - POT_MIN) *
            positions) / (POT_MAX - POT_MIN + 1));
    };
    low_position_ = PotToScalePosition(
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_2));
    high_position_ = PotToScalePosition(
        Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_6));
    low_ = static_cast<int>(std::round(low_position_));
    high_ = static_cast<int>(std::round(high_position_));
    rhythm_ = quantize(Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_3), kRhythmPositions);
    GenerateNewHome(false);
    startup_home_ready_ = true;
}

void AppTemplate::AddPitchDeparture()
{
    static constexpr std::array<size_t, 6>
        mutable_positions = {
            1,
            2,
            3,
            4,
            5,
            6
        };


    size_t position =
        mutable_positions[
            std::rand() %
            mutable_positions.size()
        ];


    int next_distance =
        current_distance_ + 1;


    int magnitude = 1;


    if (next_distance >= 2)
    {
        if ((std::rand() % 2) == 0)
        {
            magnitude = 2;
        }
    }


    if (next_distance >= 7)
    {
        int roll =
            std::rand() % 100;

        if (roll < 35)
        {
            magnitude = 3;
        }
        else if (roll < 75)
        {
            magnitude = 2;
        }
    }


    if (next_distance >= 10)
    {
        int roll =
            std::rand() % 100;

        if (roll < 35)
        {
            magnitude = 4;
        }
        else if (roll < 75)
        {
            magnitude = 3;
        }
        else
        {
            magnitude = 2;
        }
    }


    int direction =
        (std::rand() % 2 == 0)
            ? -1
            : 1;


    current_pitch_[position] =
        ClampPitchDegree(
            current_pitch_[position] +
            direction * magnitude);
}


// ------------------------------------------------------------
// LOCAL RHYTHM DEPARTURE
// ------------------------------------------------------------

void AppTemplate::AddRhythmDeparture()
{
    std::array<size_t, kPhraseLength> decoration_positions{};
    size_t decoration_count = 0;
    for (size_t i = 1; i < kPhraseLength - 1; ++i)
        if (!home_rhythm_[i]) decoration_positions[decoration_count++] = i;
    if (decoration_count == 0) return;

    const size_t position =
        decoration_positions[std::rand() % decoration_count];


    bool proposed =
        !current_rhythm_[position];


    if (proposed &&
        !home_rhythm_[position])
    {
        int extra = 0;


        if (current_distance_ >= 6)
        {
            extra =
                (std::rand() % 3) - 1;
        }


        current_pitch_[position] =
            ClampPitchDegree(
                home_pitch_[position] +
                extra);
    }


    current_rhythm_[position] =
        proposed;
}


// ------------------------------------------------------------
// COORDINATED CONTOUR DEPARTURE
// ------------------------------------------------------------

void AppTemplate::AddContourDeparture()
{
    size_t start =
        1 + (std::rand() % 3);

    int length =
        2 + (std::rand() % 3);

    int direction =
        (std::rand() % 2 == 0)
            ? -1
            : 1;


    int magnitude = 1;


    if (current_distance_ >= 7)
    {
        magnitude =
            1 + (std::rand() % 2);
    }


    if (current_distance_ >= 10)
    {
        magnitude =
            2 + (std::rand() % 2);
    }


    for (int offset = 0;
         offset < length;
         offset++)
    {
        size_t position =
            start + offset;


        if (position >=
            kPhraseLength - 1)
        {
            break;
        }


        int local_magnitude =
            magnitude;


        if (offset > 0 &&
            (std::rand() % 3) == 0)
        {
            local_magnitude++;
        }


        current_pitch_[position] =
            ClampPitchDegree(
                current_pitch_[position] +
                direction *
                local_magnitude);
    }
}


// ------------------------------------------------------------
// STRONG RHYTHMIC RESTRUCTURE
// ------------------------------------------------------------

void AppTemplate::AddRhythmRestructure()
{
    int changes = 2;


    if (current_distance_ >= 10)
    {
        changes = 3;
    }


    for (int change = 0;
         change < changes;
         change++)
    {
        std::array<size_t, kPhraseLength> decoration_positions{};
        size_t decoration_count = 0;
        for (size_t i = 1; i < kPhraseLength - 1; ++i)
            if (!home_rhythm_[i]) decoration_positions[decoration_count++] = i;
        if (decoration_count == 0) return;

        const size_t position =
            decoration_positions[std::rand() % decoration_count];


        bool proposed =
            !current_rhythm_[position];


        if (proposed &&
            !current_rhythm_[position])
        {
            int pitch_source =
                home_pitch_[position];


            if (current_distance_ >= 10)
            {
                pitch_source +=
                    (std::rand() % 5) - 2;
            }


            current_pitch_[position] =
                ClampPitchDegree(
                    pitch_source);
        }


        current_rhythm_[position] =
            proposed;
    }
}


// ------------------------------------------------------------
// REGISTER DEPARTURE
// ------------------------------------------------------------

void AppTemplate::AddRegisterDeparture(bool upward)
{
    int direction =
        upward ? 1 : (std::rand() % 2 == 0)
            ? -1
            : 1;

    // Retire the legacy LOW presentation while consuming its former draw.
    // This keeps the existing composition RNG sequence byte-for-byte aligned.
    const int wander = std::max(current_distance_, requested_distance_);
    if (direction < 0 && wander >= 7 && wander <= 11)
        (void)(std::rand() % 100);

    int amount =
        (current_distance_ >= 10)
            ? 4
            : 3;


    for (size_t i = 1;
         i < kPhraseLength - 1;
         i++)
    {
        current_pitch_[i] =
            ClampPitchDegree(
                current_pitch_[i] +
                direction * amount);
    }
}


// ------------------------------------------------------------
// ONE STEP OUTWARD
// ------------------------------------------------------------

void AppTemplate::StepOutward()
{
    ResetLivingState();


    int next_distance =
        current_distance_ + 1;


    // --------------------------------------------------------
    // NEAR: 1–2
    // --------------------------------------------------------

    if (next_distance <= 2)
    {
        if ((std::rand() % 100) < 25)
        {
            AddRhythmDeparture();
        }
        else
        {
            AddPitchDeparture();
        }
    }


    // --------------------------------------------------------
    // DEVELOPING: 3–6
    // --------------------------------------------------------

    else if (next_distance <= 6)
    {
        for (int i = 0;
             i < 2;
             i++)
        {
            int roll =
                std::rand() % 100;


            if (roll < 35)
            {
                AddRhythmDeparture();
            }
            else if (roll < 85)
            {
                AddPitchDeparture();
            }
            else
            {
                AddContourDeparture();
            }
        }
    }


    // --------------------------------------------------------
    // FAR: 7–9
    // --------------------------------------------------------

    else if (next_distance <= 9)
    {
        for (int i = 0;
             i < 3;
             i++)
        {
            int roll =
                std::rand() % 100;


            if (roll < 25)
            {
                AddPitchDeparture();
            }
            else if (roll < 45)
            {
                AddRhythmDeparture();
            }
            else if (roll < 75)
            {
                AddContourDeparture();
            }
            else if (roll < 95)
            {
                AddRhythmRestructure();
            }
            else
            {
                AddRegisterDeparture();
            }
        }
    }


    // --------------------------------------------------------
    // WILDERNESS: 10–12
    // --------------------------------------------------------

    else
    {
        for (int i = 0;
             i < 4;
             i++)
        {
            int roll =
                std::rand() % 100;


            if (roll < 15)
            {
                AddPitchDeparture();
            }
            else if (roll < 30)
            {
                AddRhythmDeparture();
            }
            else if (roll < 55)
            {
                AddContourDeparture();
            }
            else if (roll < 80)
            {
                AddRhythmRestructure();
            }
            else
            {
                AddRegisterDeparture();
            }
        }
    }


    EnforceRhythmGrammar();
    if (next_distance == 12)
        AddRegisterDeparture(true);
    current_distance_ =
        next_distance;
}


// ------------------------------------------------------------
// RESTORE ONE PITCH DIFFERENCE
// ------------------------------------------------------------

bool AppTemplate::RestoreOnePitchDifference()
{
    std::array<size_t, kPhraseLength>
        differences{};

    size_t difference_count = 0;


    for (size_t i = 0;
         i < kPhraseLength;
         i++)
    {
        if (current_pitch_[i] !=
            home_pitch_[i])
        {
            differences[
                difference_count++] = i;
        }
    }


    if (difference_count == 0)
    {
        return false;
    }


    size_t position =
        differences[
            std::rand() %
            difference_count
        ];


    int current =
        current_pitch_[position];

    int home =
        home_pitch_[position];


    current = ClampPitchDegree(current);
    if (current < home) current++;
    else if (current > home) current--;

    current_pitch_[position] = current;


    return true;
}


// ------------------------------------------------------------
// RESTORE ONE RHYTHM DIFFERENCE
// ------------------------------------------------------------

bool AppTemplate::RestoreOneRhythmDifference()
{
    std::array<size_t, kPhraseLength>
        differences{};

    size_t difference_count = 0;


    for (size_t i = 1;
         i < kPhraseLength - 1;
         i++)
    {
        if (current_rhythm_[i] !=
            home_rhythm_[i])
        {
            differences[
                difference_count++] = i;
        }
    }


    if (difference_count == 0)
    {
        return false;
    }


    size_t position =
        differences[
            std::rand() %
            difference_count
        ];


    current_rhythm_[position] =
        home_rhythm_[position];


    return true;
}


// ------------------------------------------------------------
// ONE STEP INWARD
// ------------------------------------------------------------

void AppTemplate::StepInward()
{
    ResetLivingState();


    int restoration_attempts = 1;


    if (current_distance_ >= 10)
    {
        restoration_attempts = 4;
    }
    else if (current_distance_ >= 7)
    {
        restoration_attempts = 3;
    }
    else if (current_distance_ >= 4)
    {
        restoration_attempts = 2;
    }


    for (int attempt = 0;
         attempt < restoration_attempts;
         attempt++)
    {
        bool has_pitch_difference = false;
        bool has_rhythm_difference = false;


        for (size_t i = 0;
             i < kPhraseLength;
             i++)
        {
            if (current_pitch_[i] !=
                home_pitch_[i])
            {
                has_pitch_difference = true;
            }


            if (current_rhythm_[i] !=
                home_rhythm_[i])
            {
                has_rhythm_difference = true;
            }
        }


        if (has_pitch_difference &&
            has_rhythm_difference)
        {
            if ((std::rand() % 2) == 0)
            {
                RestoreOnePitchDifference();
            }
            else
            {
                RestoreOneRhythmDifference();
            }
        }
        else if (has_pitch_difference)
        {
            RestoreOnePitchDifference();
        }
        else if (has_rhythm_difference)
        {
            RestoreOneRhythmDifference();
        }
        else
        {
            break;
        }
    }


    current_distance_--;


    if (current_distance_ <= 0)
    {
        RestoreHome();
    }
}


// ------------------------------------------------------------
// LIVING DISTANCE — HOLD TIMES
// ------------------------------------------------------------

int AppTemplate::GetLivingHomeHold() const
{
    // Number of complete additional phrases for which the LOCAL
    // IDEA is allowed to speak before Attension varies it.
    //
    // Near HOME: patient.
    // Far away: more restless.

    if (current_distance_ <= 3)
    {
        return 3 + (std::rand() % 3);
    }

    if (current_distance_ <= 6)
    {
        return 2 + (std::rand() % 3);
    }

    if (current_distance_ <= 9)
    {
        return 1 + (std::rand() % 3);
    }

    return 1 + (std::rand() % 2);
}


int AppTemplate::GetLivingVariationHold() const
{
    // Variations themselves also get repeated.
    //
    // This is crucial: a new phrase should be heard as an IDEA,
    // not immediately discarded as another random event.

    if (current_distance_ <= 3)
    {
        return 2 + (std::rand() % 2);
    }

    if (current_distance_ <= 6)
    {
        return 1 + (std::rand() % 3);
    }

    return 1 + (std::rand() % 2);
}


// ------------------------------------------------------------
// LIVING DISTANCE — INITIALIZE LOCAL IDEA
// ------------------------------------------------------------

void AppTemplate::InitializeLivingState()
{
    local_idea_rhythm_ =
        current_rhythm_;

    local_idea_pitch_ =
        current_pitch_;

    local_idea_timing_samples_ =
        current_timing_samples_;


    living_initialized_ = true;

    living_variation_active_ = false;
    consecutive_trivial_variations_ = 0;

    living_hold_phrases_ =
        GetLivingHomeHold();
}


// ------------------------------------------------------------
// LIVING DISTANCE — CREATE VARIATION
// ------------------------------------------------------------

bool AppTemplate::IsTrivialLivingVariation() const
{
    uint8_t changed_attacked_pitches = 0;
    for (size_t step = 0; step < kPhraseLength; ++step)
    {
        if (current_rhythm_[step] != local_idea_rhythm_[step])
            return false;
        if (!current_rhythm_[step])
            continue;

        const int pitch_difference =
            current_pitch_[step] - local_idea_pitch_[step];
        if (pitch_difference == 0)
            continue;
        if (pitch_difference != -1 && pitch_difference != 1)
            return false;
        if (++changed_attacked_pitches > 1)
            return false;
    }
    return changed_attacked_pitches == 1;
}

void AppTemplate::GenerateLivingVariationCandidate()
{
    // Every living variation begins from the LOCAL IDEA.
    //
    // This is the anti-drift rule.
    //
    // Attension may repeatedly explore around its current
    // territory, but parked DISTANCE cannot silently accumulate
    // mutation forever.

    current_rhythm_ =
        local_idea_rhythm_;

    current_pitch_ =
        local_idea_pitch_;

    current_timing_samples_ =
        local_idea_timing_samples_;


    int transformations = 1;


    if (current_distance_ >= 4)
    {
        transformations = 1 + (std::rand() % 2);
    }


    if (current_distance_ >= 7)
    {
        transformations = 2;
    }


    if (current_distance_ >= 10)
    {
        transformations =
            2 + (std::rand() % 2);
    }


    for (int i = 0;
         i < transformations;
         i++)
    {
        int roll =
            std::rand() % 100;


        // ----------------------------------------------------
        // NEAR HOME
        // ----------------------------------------------------

        if (current_distance_ <= 3)
        {
            if (roll < 80)
            {
                AddPitchDeparture();
            }
            else
            {
                AddRhythmDeparture();
            }
        }


        // ----------------------------------------------------
        // DEVELOPING
        // ----------------------------------------------------

        else if (current_distance_ <= 6)
        {
            if (roll < 45)
            {
                AddPitchDeparture();
            }
            else if (roll < 75)
            {
                AddRhythmDeparture();
            }
            else
            {
                AddContourDeparture();
            }
        }


        // ----------------------------------------------------
        // FAR
        // ----------------------------------------------------

        else if (current_distance_ <= 9)
        {
            if (roll < 30)
            {
                AddPitchDeparture();
            }
            else if (roll < 50)
            {
                AddRhythmDeparture();
            }
            else if (roll < 80)
            {
                AddContourDeparture();
            }
            else
            {
                AddRhythmRestructure();
            }
        }


        // ----------------------------------------------------
        // WILDERNESS
        // ----------------------------------------------------

        else
        {
            if (roll < 20)
            {
                AddPitchDeparture();
            }
            else if (roll < 35)
            {
                AddRhythmDeparture();
            }
            else if (roll < 65)
            {
                AddContourDeparture();
            }
            else
            {
                AddRhythmRestructure();
            }
        }
    }


    EnforceRhythmGrammar();
}

void AppTemplate::GenerateLivingVariation()
{
    GenerateLivingVariationCandidate();
    bool trivial = IsTrivialLivingVariation();

    // Two tiny developments are still allowed to speak. Only the next
    // trivial candidate receives one fresh try from the same local idea.
    if (consecutive_trivial_variations_ >= 2 && trivial)
    {
        GenerateLivingVariationCandidate();
        trivial = IsTrivialLivingVariation();
    }

    if (trivial)
        consecutive_trivial_variations_ = std::min<uint8_t>(
            2, static_cast<uint8_t>(consecutive_trivial_variations_ + 1));
    else
        consecutive_trivial_variations_ = 0;

    living_variation_active_ = true;

    living_hold_phrases_ =
        GetLivingVariationHold();
}


// ------------------------------------------------------------
// LIVING DISTANCE — PROCESS PARKED BEHAVIOUR
// ------------------------------------------------------------

void AppTemplate::ProcessLivingDistance()
{
    // DISTANCE zero remains exact HOME.
    //
    // Zero is not merely "close to HOME." It means HOME.

    if (current_distance_ == 0)
    {
        if (current_rhythm_ != home_rhythm_ ||
            current_pitch_ != home_pitch_ ||
            current_timing_samples_ != home_timing_samples_)
        {
            RestoreHome();
        }

        return;
    }


    // First phrase after arriving establishes a LOCAL IDEA.

    if (!living_initialized_)
    {
        InitializeLivingState();
        return;
    }


    // Musical patience.
    //
    // Repeat the current idea before making another decision.

    if (living_hold_phrases_ > 0)
    {
        living_hold_phrases_--;
        return;
    }


    if (living_variation_active_)
    {
        // At FAR/WILDERNESS distances Attension sometimes moves
        // directly from one variation to another.
        //
        // IMPORTANT:
        // each new variation STILL starts from LOCAL IDEA, so
        // this sounds adventurous without accumulating drift.

        int chain_chance = 0;


        if (current_distance_ >= 10)
        {
            chain_chance = 40;
        }
        else if (current_distance_ >= 7)
        {
            chain_chance = 25;
        }
        else if (current_distance_ >= 4)
        {
            chain_chance = 10;
        }


        if ((std::rand() % 100) <
            chain_chance)
        {
            GenerateLivingVariation();
            return;
        }


        // Return to the remembered LOCAL IDEA.

        current_rhythm_ =
            local_idea_rhythm_;

        current_pitch_ =
            local_idea_pitch_;

        current_timing_samples_ =
            local_idea_timing_samples_;

        living_variation_active_ = false;

        living_hold_phrases_ =
            GetLivingHomeHold();

        return;
    }


    // We have repeated the LOCAL IDEA long enough.
    //
    // Develop it.

    GenerateLivingVariation();
}


// ------------------------------------------------------------
// PROCESS DISTANCE JOURNEY
// ------------------------------------------------------------

void AppTemplate::PlanOrdinaryWanderRegisterPresentation()
{
    // This is a phrase presentation only: LOCAL IDEA and the planned WANDER
    // melody remain intact.  HOME, W0-W4, and W12 own their existing paths.
    if (current_distance_ != requested_distance_ ||
        current_distance_ < 5 || current_distance_ > 11 ||
        home_refrain_active_)
        return;

    bool has_voice_a_note = false;
    float lowest_frequency = 0.0f;
    float highest_frequency = 0.0f;
    for (size_t step = 0; step < kPhraseLength; ++step)
    {
        if (!current_rhythm_[step])
            continue;

        const float frequency = ScaleDegreeToFrequency(
            ClampPitchDegree(current_pitch_[step]));
        if (!has_voice_a_note)
        {
            lowest_frequency = frequency;
            highest_frequency = frequency;
            has_voice_a_note = true;
        }
        else
        {
            lowest_frequency = std::min(lowest_frequency, frequency);
            highest_frequency = std::max(highest_frequency, frequency);
        }
    }
    if (!has_voice_a_note)
        return;

    // Count only eligible ordinary WANDER phrases; special material does not
    // spend the three-phrase separation between register discoveries.
    if (wander_register_cooldown_ > 0)
    {
        --wander_register_cooldown_;
        return;
    }

    // Release values: LOW/HIGH chance for W5...W11.
    static constexpr uint8_t kReleaseRegisterChance[] =
        {3, 4, 6, 8, 10, 12, 14};
    const uint8_t chance = kReleaseRegisterChance[current_distance_ - 5];

    wander_register_rng_ ^= wander_register_rng_ << 13;
    wander_register_rng_ ^= wander_register_rng_ >> 17;
    wander_register_rng_ ^= wander_register_rng_ << 5;
    const uint32_t roll = wander_register_rng_ % 100u;
    const HomePresentationRegister presentation = roll < chance ?
        HomePresentationRegister::LOW : roll < chance * 2u ?
        HomePresentationRegister::HIGH : HomePresentationRegister::NORMAL;
    if (presentation == HomePresentationRegister::NORMAL)
        return;

    // The project has no separate voice-range policy.  These conservative
    // audition guards keep the resulting Voice-A octave within 110..1760 Hz
    // (A2..A6): a LOW phrase cannot become muddy, and HIGH cannot become
    // piercing.  Safety rejects the selected direction; it never substitutes.
    constexpr float kRegisterFloorHz = 110.0f;
    constexpr float kRegisterCeilingHz = 1760.0f;
    const bool safe = presentation == HomePresentationRegister::LOW ?
        lowest_frequency * 0.5f >= kRegisterFloorHz :
        highest_frequency * 2.0f <= kRegisterCeilingHz;
    if (!safe)
        return;

    ordinary_wander_register_ = presentation;
    wander_register_cooldown_ = 3;
}

void AppTemplate::ProcessDistanceJourney()
{
    // Ordinary WANDER register presentation is phrase-owned. A new phrase
    // starts neutral unless its own register-departure planning claims it.
    ordinary_wander_register_ = HomePresentationRegister::NORMAL;

    if (current_distance_ <
        requested_distance_)
    {
        const bool leaving_home = current_distance_ == 0;
        home_refrain_active_ = false;
        home_refrain_phrases_remaining_ = 0;
        ClearStructuredHomePresentation();
        if (leaving_home)
        {
            normal_refrains_since_home_ = 0;
            // The six complete distant refrains after leaving actual HOME are
            // always development-only, independent of any prior presentation.
            home_refrain_defer_ = 6;
        }
        else
        {
            // Preserve structural progress, but let a new destination speak
            // for two complete refrains before HOME can return.
            home_refrain_defer_ = 2;
        }
        // WANDER is a destination control: compose each existing journey
        // step now so the next refrain begins at the requested territory.
        while (current_distance_ < requested_distance_)
            StepOutward();
        PlanOrdinaryWanderRegisterPresentation();
        return;
    }


    if (current_distance_ >
        requested_distance_)
    {
        home_refrain_active_ = false;
        home_refrain_phrases_remaining_ = 0;
        ClearStructuredHomePresentation();
        if (requested_distance_ == 0)
        {
            normal_refrains_since_home_ = 0;
            home_refrain_defer_ = 0;
        }
        else
        {
            home_refrain_defer_ = 2;
        }
        // The current WANDER setting owns the next phrase's legal territory.
        // Reconcile the stored journey at this boundary; the sounding phrase
        // has already finished, so no mid-phrase pitch is altered.
        while (current_distance_ > requested_distance_)
            StepInward();
        PlanOrdinaryWanderRegisterPresentation();
        return;
    }


    // Exact HOME has no need for a structural HOME presentation. Reset the
    // development cycle so leaving HOME starts with six clear refrains.
    if (current_distance_ == 0)
    {
        ResetVoiceARefrainState();
        ProcessLivingDistance();
        return;
    }

    if (home_refrain_active_)
    {
        if (--home_refrain_phrases_remaining_ == 0)
        {
            home_refrain_active_ = false;
            normal_refrains_since_home_ = 0;
            ClearStructuredHomePresentation();
        }
        else
        {
            ++structured_home_phrase_index_;
            AdvanceStructuredHomePresentation();
        }
        return;
    }

    if (normal_refrains_since_home_ < 12)
        ++normal_refrains_since_home_;

    if (home_refrain_defer_ > 0)
    {
        --home_refrain_defer_;
        ProcessLivingDistance();
        PlanOrdinaryWanderRegisterPresentation();
        return;
    }

    uint8_t chance = 0;
    switch (normal_refrains_since_home_)
    {
    case 7: chance = 10; break;
    case 8: chance = 20; break;
    case 9: chance = 35; break;
    case 10: chance = 50; break;
    case 11: chance = 70; break;
    default: break;
    }

    const auto next_refrain_random = [this]() -> uint32_t {
        voice_a_refrain_rng_ ^= voice_a_refrain_rng_ << 13;
        voice_a_refrain_rng_ ^= voice_a_refrain_rng_ >> 17;
        voice_a_refrain_rng_ ^= voice_a_refrain_rng_ << 5;
        return voice_a_refrain_rng_;
    };
    const bool start_home = normal_refrains_since_home_ >= 12 ||
        (chance > 0 && (next_refrain_random() % 100u) < chance);
    if (start_home)
    {
        const uint32_t duration_roll = next_refrain_random() % 100u;
        home_refrain_active_ = true;
        consecutive_trivial_variations_ = 0;
        home_refrain_phrases_remaining_ = duration_roll < 50u ? 2 :
            (duration_roll < 85u ? 3 : 4);
        BeginStructuredHomePresentation(home_refrain_phrases_remaining_);
        return;
    }

    ProcessLivingDistance();
    PlanOrdinaryWanderRegisterPresentation();
}


// ------------------------------------------------------------
// INITIALIZATION
// ------------------------------------------------------------

void AppTemplate::Init()
{
    inited_ = false;
    mode_ = Mode::FIRST;


    // HOME is composed after the physical controls are initialized.
    home_rhythm_.fill(false);
    home_pitch_.fill(0);
    home_timing_samples_.fill(0);
    phrase_step_ = 0;
    requested_distance_ = 0;
    current_distance_ = 0;
    ending_led_frozen_ = false;
    ending_led_wander_value_ = 0;
    ending_led_tension_ = 0;
    living_initialized_ = false;
    living_variation_active_ = false;
    living_hold_phrases_ = 0;

    // --------------------------------------------------------
    // SOUND ENGINE
    // --------------------------------------------------------

    osc_.Init(SAMPLE_RATE);

    osc_.SetFrequency(
        ScaleDegreeToFrequency(0));


    env_.Init(SAMPLE_RATE);

    env_.SetNonResetting(
        AdsrEnv::NonResetting::DECAY);

    env_value_ = 0;


    // --------------------------------------------------------
    // HARMONY VOICE
    // --------------------------------------------------------
    //
    // Voice B has its own oscillator and envelope; its character is configured
    // during voice setup.

    harmony_osc_.Init(SAMPLE_RATE);

    harmony_osc_.SetFrequency(
        ScaleDegreeToFrequency(2));


    harmony_env_.Init(SAMPLE_RATE);

    harmony_env_.SetNonResetting(
        AdsrEnv::NonResetting::DECAY);
    harmony_env_.SetReleaseTime(0.18f);

    harmony_env_value_ = 0;
    last_voice_b_midi_note_ = -1;

    voice_a_layer_osc_.Init(SAMPLE_RATE);
    voice_a_fm_.Init(SAMPLE_RATE);
    voice_a_oddity_glint_.Init(SAMPLE_RATE);
    voice_b_layer_osc_.Init(SAMPLE_RATE);
    voice_b_fm_.Init(SAMPLE_RATE);
    voice_b_oddity_glint_.Init(SAMPLE_RATE);

    // HOME now has a stable sonic identity as well as a melodic one.
    SetVoiceACharacter(active_voice_a_);
    const auto &initial_voice_a_definition =
        kVoicePalette[static_cast<size_t>(active_voice_a_)];
    voice_a_layer_level_ = static_cast<q15_t>(
        initial_voice_a_definition.layer_level * Q15_MAX);
    SetVoiceAOutputLevel(active_voice_a_);
    voice_a_uses_fm_ = initial_voice_a_definition.fm_index > 0.0f;
    SetVoiceBCharacter(active_voice_b_);


    // --------------------------------------------------------
    // ATMOSPHERE
    // --------------------------------------------------------
    // Long, soft harmonic bed rather than another note-by-note line.

    atmosphere_osc_.Init(SAMPLE_RATE);
    atmosphere_osc_.SetWaveform(Oscillator::Waveform::TRI);
    atmosphere_osc_.SetFrequency(ScaleDegreeToFrequency(0));

    atmosphere_env_.Init(SAMPLE_RATE);
    atmosphere_env_.SetAttackTime(0.18f);
    atmosphere_env_.SetDecayTime(2.80f);
    atmosphere_env_.SetNonResetting(AdsrEnv::NonResetting::DECAY);

    atmosphere_env_value_ = 0;
    atmosphere_degree_ = 0;
    atmosphere_active_ = false;

    atmosphere_noise_.Seed(0x4E71A2D3u);
    atmosphere_tone_a_.Init(SAMPLE_RATE);
    atmosphere_tone_b_.Init(SAMPLE_RATE);
    atmosphere_tone_a_.SetWaveform(Oscillator::Waveform::SINE);
    atmosphere_tone_b_.SetWaveform(Oscillator::Waveform::SINE);
    atmosphere_whistle_osc_.Init(SAMPLE_RATE);
    atmosphere_whistle_osc_.SetWaveform(Oscillator::Waveform::SINE);
    atmosphere_whistle_osc_.SetFrequency(ScaleDegreeToFrequency(9));
    atmosphere_whistle_level_ = Q15_MAX;
    atmosphere_whistle_ending_divider_ = 0;
    atmosphere_recent_degree_ = 0;
    atmosphere_previous_degree_ = 0;
    atmosphere_position_ = 0;
    atmosphere_position_target_ = 0;
    atmosphere_effect_strength_ = Q15_MAX / 20;
    atmosphere_fracture_weight_ = Q15_MAX;
    atmosphere_lfo_phase_ = 0;
    atmosphere_fracture_gate_level_ = Q15_MAX;
    atmosphere_fracture_gate_floor_ = Q15_MAX;
    atmosphere_fracture_gate_age_ = 0;
    atmosphere_fracture_gate_cut_start_ = 0;
    atmosphere_fracture_gate_release_ = 0;
    atmosphere_fracture_gate_decrement_ = 0;
    atmosphere_fracture_gate_active_ = false;
    atmosphere_event_note_ = 0;
    atmosphere_target_ = 0;
    atmosphere_level_ = 0;
    atmosphere_delay_pos_ = atmosphere_delay_history_ = 0;
    atmosphere_reverb_left_.fill(0);
    atmosphere_reverb_right_.fill(0);
    atmosphere_reverb_left_pos_ = atmosphere_reverb_right_pos_ = 0;
    atmosphere_reverb_damp_left_ = atmosphere_reverb_damp_right_ = 0;
    atmosphere_reverb_field_left_ = atmosphere_reverb_field_right_ = 0;
    dream_a_current_ = dream_a_target_ = 0;
    dream_a_layer_current_ = dream_a_layer_target_ = 0;
    dream_b_current_ = dream_b_target_ = 0;
    dream_b_layer_current_ = dream_b_layer_target_ = 0;
    dream_a_ready_ = dream_b_ready_ = false;
    atmosphere_event_state_ = 0xA53C91E7u;
    atmosphere_wait_samples_ = SAMPLE_RATE;
    atmosphere_event_samples_ = 0;
    atmosphere_event_age_ = 0;
    atmosphere_filter_ = 0;
    atmosphere_event_kind_ = 0;


    // --------------------------------------------------------
    // PERCUSSION
    // --------------------------------------------------------
    //
    // A low sine thump rather than a full drum kit.  Short decay,
    // no sustain behaviour, and conservative mixer level.

    percussion_osc_.Init(SAMPLE_RATE);

    percussion_osc_.SetWaveform(
        Oscillator::Waveform::SINE);

    percussion_osc_.SetFrequency(
        82.0f);


    percussion_env_.Init(SAMPLE_RATE);

    percussion_env_.SetAttackTime(0.001f);
    percussion_env_.SetDecayTime(0.140f);

    percussion_env_.SetNonResetting(
        AdsrEnv::NonResetting::DECAY);

    percussion_env_value_ = 0;

    rhythm_detent_body_osc_.Init(SAMPLE_RATE);
    rhythm_detent_body_osc_.SetWaveform(Oscillator::Waveform::TRI);
    rhythm_detent_body_osc_.SetFrequency(650.0f);
    rhythm_detent_glint_osc_.Init(SAMPLE_RATE);
    rhythm_detent_glint_osc_.SetWaveform(Oscillator::Waveform::SINE);
    rhythm_detent_glint_osc_.SetFrequency(1950.0f);
    rhythm_detent_body_remaining_ = 0;
    rhythm_detent_glint_delay_ = 0;
    rhythm_detent_glint_remaining_ = 0;


    // --------------------------------------------------------
    // HI-HAT
    // --------------------------------------------------------
    //
    // Short noise ticks provide a clear phrase-grid reference around
    // ATTENSION's organic melodic timing.

    hihat_env_.Init(SAMPLE_RATE);

    hihat_env_.SetAttackTime(0.001f);
    hihat_env_.SetDecayTime(0.024f);

    hihat_env_.SetNonResetting(
        AdsrEnv::NonResetting::DECAY);

    hihat_env_value_ = 0;
    hihat_noise_state_ = 0x7A3C59E1u;
    snare_env_.Init(SAMPLE_RATE);
    snare_env_.SetAttackTime(0.001f);
    snare_env_.SetDecayTime(0.070f);
    snare_env_.SetNonResetting(AdsrEnv::NonResetting::DECAY);
    snare_env_value_ = 0;
    // HOME hat: simple alternating pulse.
    hihat_pattern_mask_ = 0x55u;
    hihat_fill_style_ = 0;
    hihat_phrase_replans_ = 0;
    cymbal_restraint_phrases_remaining_ = 0;
    ArmNextCymbalRestraint();

    samples_since_clock_ = 0;
    last_clock_interval_samples_ = 0;
    clock_timing_sync_type_ = Clock::Sync::COUNT;
    clock_timing_running_ = false;
    hihat_subhit_1_samples_ = -1;
    hihat_subhit_2_samples_ = -1;
    hihat_subhit_3_samples_ = -1;
    hihat_subhit_1_open_ = false;
    drum_transition_ = DrumTransition::NONE;
    transition_open_hat_pending_ = false;

    percussion_fill_length_ = 0;


    // --------------------------------------------------------
    // NORMAL PERFORMANCE
    // --------------------------------------------------------
    //
    // AMOUNT (centre knob) = WANDER
    //
    // Fully CCW is HOME. Clockwise grants progressively more
    // compositional territory. TIME now controls TENSION.

    distance_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_4,
        .layer = Hardware::Layer::NORMAL,
        .map_size = kPhysicalWanderPositions,
    });

    distance_pot_->Init(
        AUDIO_LOOP_RATE);


    // Pot 3 is musical TENSION; Pot 2 independently controls ATMOS depth.
    tension_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_3,
        .layer = Hardware::Layer::NORMAL,
        .map_size = kTensionPositions,
    });

    tension_pot_->Init(
        AUDIO_LOOP_RATE);

    atmosphere_depth_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_2,
        .layer = Hardware::Layer::NORMAL,
        .map_size = 256,
    });
    atmosphere_depth_pot_->Init(AUDIO_LOOP_RATE);

    voice_a_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_5,
        .layer = Hardware::Layer::NORMAL,
        .map_size = kVoicePositions + 1, // OFF + ten finished characters.
    });

    voice_a_pot_->Init(
        AUDIO_LOOP_RATE);

    voice_b_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_6,
        .layer = Hardware::Layer::NORMAL,
        .map_size = kVoicePositions + 1,
    });

    voice_b_pot_->Init(
        AUDIO_LOOP_RATE);

    atmosphere_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_1,
        .layer = Hardware::Layer::NORMAL,
    });

    atmosphere_pot_->Init(
        AUDIO_LOOP_RATE);


    // --------------------------------------------------------
    // HOME BUILDER — REVISED CONTROL MAP
    // --------------------------------------------------------
    //
    // SHIFT + FEEDBACK MOD = CONTOUR
    // SHIFT + LFO MOD      = RHYTHM
    // SHIFT + FEEDBACK     = RANGE
    //
    // TIME and TIME MOD are deliberately left alone so Bastl's
    // output/headphone gain and input gain controls remain intact.

    low_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_2,
        .layer = Hardware::Layer::SHIFT,
        .initial_value = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_2),
        .map_size = kPitchPositions,
    });
    low_pot_->Init(AUDIO_LOOP_RATE);
    rhythm_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_3,
        .layer = Hardware::Layer::SHIFT,
        .initial_value = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_3),
        .map_size = kRhythmPositions,
    });
    rhythm_pot_->Init(AUDIO_LOOP_RATE);
    high_pot_ = FancyPot::Create({
        .pot = Hardware::Pot::POT_6,
        .layer = Hardware::Layer::SHIFT,
        .initial_value = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_6),
        .map_size = kPitchPositions,
    });
    high_pot_->Init(AUDIO_LOOP_RATE);
    ReadStartupHomeControls();
    Kastle2::base.SetTapTempoEnabled(false);
    // ATTENSION owns Pot 3 / PARAM2.  Base continues to generate its LFO
    // and TRI/PULSE outputs, but must not treat this patch path as LFO rate.
    Kastle2::base.SetFeatureEnabled(Base::Feature::LFO_MOD_MAPPING, false);

    Kastle2::base.SetFeatureEnabled(
        Base::Feature::AUDIO_CHAIN,
        true);


    inited_ = true;
}


void AppTemplate::DeInit()
{
    Kastle2::base.SetTapTempoEnabled(true);
    Kastle2::base.SetFeatureEnabled(Base::Feature::LFO_MOD_MAPPING, true);
    inited_ = false;
}


// ------------------------------------------------------------
// AUDIO / COMPOSITION LOOP
// ------------------------------------------------------------

void AppTemplate::SetDreamTargets(
    float frequency,
    float layer_ratio,
    q31_t &current,
    q31_t &target,
    q31_t &layer_current,
    q31_t &layer_target,
    bool &ready)
{
    const q31_t next = freq_to_q31(frequency, SAMPLE_RATE);
    const q31_t next_layer = freq_to_q31(frequency * layer_ratio, SAMPLE_RATE);
    const int32_t depth = std::max<int32_t>(atmosphere_level_, 0);
    if (ready && depth > 0)
    {
        // At low DREAM amounts the journey begins close to the new note. At
        // full DREAM it begins at the last legitimate note and travels all
        // the way, without inventing pitches outside the composition.
        current = static_cast<q31_t>(static_cast<int64_t>(next) +
            ((static_cast<int64_t>(target) - next) * depth >> 15));
        layer_current = static_cast<q31_t>(static_cast<int64_t>(next_layer) +
            ((static_cast<int64_t>(layer_target) - next_layer) * depth >> 15));
    }
    else
    {
        current = next;
        layer_current = next_layer;
    }
    target = next;
    layer_target = next_layer;
    ready = true;
}


FASTCODE void AppTemplate::UpdateDreamGlissando()
{
    const int32_t depth = std::max<int32_t>(atmosphere_level_, 0);
    auto advance = [](q31_t &current, q31_t target, int32_t coefficient)
    {
        const int32_t difference = target - current;
        if (difference >= -2 && difference <= 2)
        {
            current = target;
            return;
        }
        int32_t step = static_cast<int32_t>(
            (static_cast<int64_t>(difference) * coefficient) >> 15);
        if (step == 0) step = difference > 0 ? 1 : -1;
        current += step;
    };

    if (dream_a_ready_)
    {
        const q31_t previous = dream_a_current_;
        const q31_t previous_layer = dream_a_layer_current_;
        if (depth == 0)
        {
            // Finish an in-flight journey quickly instead of snapping at CLEAR.
            advance(dream_a_current_, dream_a_target_, 4096);
            advance(dream_a_layer_current_, dream_a_layer_target_, 4096);
        }
        else
        {
            // More DREAM means a longer journey. A leads; B follows.
            const int32_t coefficient = 2048 - ((depth * 1928) >> 15);
            advance(dream_a_current_, dream_a_target_, coefficient);
            advance(dream_a_layer_current_, dream_a_layer_target_, coefficient);
        }
        if (dream_a_current_ != previous ||
            dream_a_layer_current_ != previous_layer)
        {
            osc_.SetNativeFrequency(dream_a_current_);
            voice_a_layer_osc_.SetNativeFrequency(dream_a_layer_current_);
            voice_a_fm_.SetNativeFrequency(dream_a_current_);
        }
    }

    if (dream_b_ready_)
    {
        const q31_t previous = dream_b_current_;
        const q31_t previous_layer = dream_b_layer_current_;
        if (depth == 0)
        {
            advance(dream_b_current_, dream_b_target_, 4096);
            advance(dream_b_layer_current_, dream_b_layer_target_, 4096);
        }
        else
        {
            const int32_t coefficient = 1536 - ((depth * 1471) >> 15);
            advance(dream_b_current_, dream_b_target_, coefficient);
            advance(dream_b_layer_current_, dream_b_layer_target_, coefficient);
        }
        if (dream_b_current_ != previous ||
            dream_b_layer_current_ != previous_layer)
        {
            harmony_osc_.SetNativeFrequency(dream_b_current_);
            voice_b_layer_osc_.SetNativeFrequency(dream_b_layer_current_);
            voice_b_fm_.SetNativeFrequency(dream_b_current_);
        }
    }
}


FASTCODE void AppTemplate::ProcessAtmosphereEnvironment(
    uint8_t environment,
    int32_t dry,
    int32_t tap70,
    int32_t tap140,
    int32_t tap280,
    int32_t whistle_tone,
    int32_t &wet_l,
    int32_t &wet_r)
{
    wet_l = wet_r = 0;
    const int32_t intensity = atmosphere_effect_strength_;
    const int32_t history = atmosphere_delay_history_;
    const auto tap = [this, history](size_t delay) -> int32_t {
        if (history < static_cast<int32_t>(delay)) return 0;
        size_t position = atmosphere_delay_pos_ + kAtmosphereDelaySamples - delay;
        if (position >= kAtmosphereDelaySamples) position -= kAtmosphereDelaySamples;
        return atmosphere_delay_[position];
    };

    switch (static_cast<AtmosphereEnvironment>(environment))
    {
    case AtmosphereEnvironment::ECHO:
    {
        // Favor later repeats rather than raising the whole wet bus.
        const int32_t echo_l = ((tap70 * (8192 + intensity / 4)) +
            (tap140 * 8192) + (tap280 * (6144 + intensity / 8))) >> 15;
        const int32_t echo_r = ((tap280 * (8192 + intensity / 4)) +
            (tap140 * 8192) + (tap70 * (6144 + intensity / 8))) >> 15;
        // Keep all tap relationships intact while scaling Echo's local wet
        // contribution to 30% before the common environment gain and Presence mix.
        static constexpr int32_t kEchoOutputScale = 9830; // 0.30 in Q15.
        wet_l = (echo_l * kEchoOutputScale) >> 15;
        wet_r = (echo_r * kEchoOutputScale) >> 15;
        break;
    }

    case AtmosphereEnvironment::FLANGE:
    {
        // Fold the 10-bit phase into a continuous 0..512..0 triangle. The
        // opposed delay mapping below is defined over 0..512, not 0..1023.
        const uint32_t phase_position = atmosphere_lfo_phase_ >> 22;
        const uint32_t sweep = phase_position <= 512 ? phase_position :
            1024 - phase_position;
        const size_t sweep_depth = 260 + (intensity >> 6);
        const size_t left_delay = 80 + static_cast<size_t>(sweep * sweep_depth >> 9);
        const size_t right_delay = 80 + static_cast<size_t>((512 - sweep) * sweep_depth >> 9);
        const int32_t left = tap(left_delay);
        const int32_t right = tap(right_delay);
        const int32_t amount = 4096 + ((intensity * 13) >> 4);
        const int32_t flange_l = ((left + (dry >> 1)) * amount) >> 15;
        const int32_t flange_r = ((right + (dry >> 1)) * amount) >> 15;
        // The safe, full-bandwidth comb is intentionally vivid, but the old
        // level was tuned through the removed wet-path low-pass. Retain its
        // geometry and internal blend while reducing only its local output.
        static constexpr int32_t kFlangeOutputScale = 13107; // 0.40 in Q15.
        wet_l = (flange_l * kFlangeOutputScale) >> 15;
        wet_r = (flange_r * kFlangeOutputScale) >> 15;
        break;
    }

    case AtmosphereEnvironment::DISTORT:
    {
        // One shared melodic path preserves the useful A+B interaction. The
        // adopted Voice B state remains active through its natural envelope
        // release, so a REST tail is never chopped merely to change drive.
        const bool voice_a_active = !voice_a_lab_off_;
        const bool voice_b_active = voice_b_lab_active_selection_ > 0;
        static constexpr int32_t kDriveAOnlyQ12 = 16384; // 4.0x
        static constexpr int32_t kDriveBOnlyQ12 = 32768; // 8.0x
        static constexpr int32_t kDriveBothQ12 = 12288;  // 3.0x
        const int32_t drive_q12 = voice_a_active && voice_b_active ?
            kDriveBothQ12 : voice_a_active ? kDriveAOnlyQ12 :
            voice_b_active ? kDriveBOnlyQ12 : 0;
        const int32_t input = std::clamp<int32_t>(dry, Q15_MIN, Q15_MAX);
        const int32_t driven = std::clamp<int32_t>(
            (input * drive_q12) >> 12, Q15_MIN, Q15_MAX);
        const int32_t square_a = (driven * driven) >> 15;
        const int32_t cube_a = (square_a * driven) >> 15;
        const int32_t stage_a = std::clamp<int32_t>(
            driven + ((driven - cube_a) >> 1), Q15_MIN, Q15_MAX);
        const int32_t square_b = (stage_a * stage_a) >> 15;
        const int32_t cube_b = (square_b * stage_a) >> 15;
        const int32_t shaped = std::clamp<int32_t>(
            stage_a + ((stage_a - cube_b) >> 1), Q15_MIN, Q15_MAX);
        static constexpr int32_t kProcessedScale = 8426; // ~0.25714 Q15.
        const int32_t processed = (shaped * kProcessedScale) >> 15;
        const int32_t correction = processed - input;
        // 2/3 here becomes unity after the common 1.5x environment gain.
        static constexpr int32_t kDistortOutputScale = 21845;
        wet_l = wet_r = (correction * kDistortOutputScale) >> 15;
        break;
    }

    case AtmosphereEnvironment::WHISTLE:
    {
        // A delayed tonal image plus a consonant octave-plus-fifth halo.
        const int32_t delayed = (tap280 * 4096) >> 15;
        const int32_t active_envelope = std::max<int32_t>(env_value_, harmony_env_value_);
        const int32_t breath = 8192 + (active_envelope >> 1);
        // A DC-corrected square of the existing sine supplies a deliberately
        // tiny second harmonic without another oscillator, FM, or filter.
        const int32_t second_harmonic = ((whistle_tone * whistle_tone) >> 15) - 16384;
        const int32_t enriched_tone = std::clamp<int32_t>(
            whistle_tone + (second_harmonic >> 5), Q15_MIN, Q15_MAX);
        const int32_t halo = (((enriched_tone * 5120) >> 15) *
            atmosphere_whistle_level_ >> 15) * breath >> 15;
        static constexpr int32_t kWhistleOutputScale = 13763;
        const int32_t whistle = (delayed + halo) * kWhistleOutputScale >> 15;
        wet_l = wet_r = whistle;
        break;
    }

    case AtmosphereEnvironment::REVERB:
    {
        // Bright, direct room view of the continuously evolving field.
        // It remains additive-only, so FLANGE -> REVERB cannot recreate the
        // retired PULSE dry-signal cancellation valley.
        const int32_t reverb_l = ((atmosphere_reverb_field_left_ * 21000) +
                 (atmosphere_reverb_field_right_ * 4500)) >> 15;
        const int32_t reverb_r = ((atmosphere_reverb_field_right_ * 21000) +
                 (atmosphere_reverb_field_left_ * 4500)) >> 15;
        static constexpr int32_t kReverbOutputScale = 24576;
        wet_l = (reverb_l * kReverbOutputScale) >> 15;
        wet_r = (reverb_r * kReverbOutputScale) >> 15;
        break;
    }

    case AtmosphereEnvironment::FRACTURE:
    {
        // FRACTURE is applied as a final multiplicative gate below. It does
        // not enter the additive wet-effect path used by other environments.
        wet_l = wet_r = 0;
        break;
    }

    default:
        break;
    }

    static constexpr int32_t kEnvironmentGain[] = {
        32767, 49152, 49152, 49152, 49152, 49152, 32767};
    const int32_t gain = kEnvironmentGain[environment];
    wet_l = (wet_l * gain >> 15) * intensity >> 15;
    wet_r = (wet_r * gain >> 15) * intensity >> 15;

}


FASTCODE void AppTemplate::ProcessAtmosphereReverbCore(int32_t dry)
{
    // This core advances once per audio sample regardless of the selected
    // environment. REVERB is a read-only output view, so its
    // crossfade cannot double-write or otherwise fight this feedback field.
    const int32_t delayed_left = atmosphere_reverb_left_[atmosphere_reverb_left_pos_];
    const int32_t delayed_right = atmosphere_reverb_right_[atmosphere_reverb_right_pos_];
    atmosphere_reverb_field_left_ = delayed_left;
    atmosphere_reverb_field_right_ = delayed_right;

    // One-pole damping lives inside the loop, progressively darkening repeats.
    atmosphere_reverb_damp_left_ +=
        (delayed_left - atmosphere_reverb_damp_left_) >> 4;
    atmosphere_reverb_damp_right_ +=
        (delayed_right - atmosphere_reverb_damp_right_) >> 4;

    // Unequal lines plus restrained crossfeed make the tail decorrelate while
    // the 0.93 total feedback bound prevents uncontrolled accumulation.
    const int32_t write_left = ((dry * 3) >> 2) +
        ((atmosphere_reverb_damp_left_ * 25000 +
          atmosphere_reverb_damp_right_ * 5500) >> 15);
    const int32_t write_right = ((dry * 3) >> 2) +
        ((atmosphere_reverb_damp_right_ * 25000 +
          atmosphere_reverb_damp_left_ * 5500) >> 15);
    atmosphere_reverb_left_[atmosphere_reverb_left_pos_] =
        static_cast<int16_t>(std::clamp<int32_t>(write_left, -30000, 30000));
    atmosphere_reverb_right_[atmosphere_reverb_right_pos_] =
        static_cast<int16_t>(std::clamp<int32_t>(write_right, -30000, 30000));
    if (++atmosphere_reverb_left_pos_ == kAtmosphereReverbDelayLeft)
        atmosphere_reverb_left_pos_ = 0;
    if (++atmosphere_reverb_right_pos_ == kAtmosphereReverbDelayRight)
        atmosphere_reverb_right_pos_ = 0;
}


FASTCODE void AppTemplate::ProcessAtmosphere(int32_t dry, int32_t &wet_l, int32_t &wet_r)
{
    // Advance the halo continuously, even while an adjacent effect owns the
    // crossfade, so entering SHIMMER never restarts its waveform phase.
    const int32_t whistle_tone = q31_to_q15(atmosphere_whistle_osc_.Process());
    const bool whistle_sources_active = !voice_a_lab_off_ ||
        (voice_b_arrangement_ != VoiceBArrangement::HOLD &&
         voice_b_lab_active_selection_ > 0 && harmony_env_value_ > 0);
    if (ended_)
    {
        // ENDED remains a hard song boundary; no WHISTLE halo outlives it.
        atmosphere_whistle_level_ = 0;
        atmosphere_whistle_ending_divider_ = 0;
    }
    else if (ending_cadence_active_)
    {
        // Release the established halo over ~2.23 seconds at 44 kHz.
        if (atmosphere_whistle_level_ > 0 &&
            ++atmosphere_whistle_ending_divider_ == 3)
        {
            atmosphere_whistle_ending_divider_ = 0;
            --atmosphere_whistle_level_;
        }
    }
    else
    {
        atmosphere_whistle_ending_divider_ = 0;
        if (!whistle_sources_active && atmosphere_whistle_level_ > 0)
        {
            // Preserve the existing short non-cadence release behavior.
            atmosphere_whistle_level_ = std::max<q15_t>(
                0, static_cast<q15_t>(atmosphere_whistle_level_ - 2));
        }
    }
    const int32_t position_delta =
        atmosphere_position_target_ - atmosphere_position_;
    if (position_delta >= -8 && position_delta <= 8)
        atmosphere_position_ = atmosphere_position_target_;
    else
    {
        int32_t position_change = position_delta / 256;
        if (position_change == 0)
            position_change = position_delta > 0 ? 1 : -1;
        atmosphere_position_ += position_change;
    }

    const int32_t scaled_position = atmosphere_position_ *
        (static_cast<int32_t>(AtmosphereEnvironment::COUNT) - 1);
    const uint8_t lower_environment = static_cast<uint8_t>(
        std::min<int32_t>(static_cast<int32_t>(AtmosphereEnvironment::COUNT) - 1,
            scaled_position / Q15_MAX));
    const uint8_t upper_environment = static_cast<uint8_t>(
        std::min<int32_t>(static_cast<int32_t>(AtmosphereEnvironment::COUNT) - 1,
            static_cast<int32_t>(lower_environment) + 1));
    const int32_t upper_weight = lower_environment == upper_environment ? 0 :
        scaled_position - static_cast<int32_t>(lower_environment) * Q15_MAX;
    const int32_t lower_weight = Q15_MAX - upper_weight;
    const int32_t fracture_weight =
        lower_environment == static_cast<uint8_t>(AtmosphereEnvironment::FRACTURE) ? lower_weight : 0;
    const int32_t upper_fracture_weight =
        upper_environment == static_cast<uint8_t>(AtmosphereEnvironment::FRACTURE) ? upper_weight : 0;
    atmosphere_fracture_weight_ = static_cast<q15_t>(
        fracture_weight + upper_fracture_weight);

    if (atmosphere_fracture_gate_active_)
    {
        if (atmosphere_fracture_gate_age_ >= atmosphere_fracture_gate_cut_start_)
        {
            atmosphere_fracture_gate_level_ = std::max<q15_t>(
                atmosphere_fracture_gate_floor_,
                static_cast<q15_t>(atmosphere_fracture_gate_level_ -
                    atmosphere_fracture_gate_decrement_));
        }
        if (++atmosphere_fracture_gate_age_ >=
            atmosphere_fracture_gate_cut_start_ + atmosphere_fracture_gate_release_)
        {
            atmosphere_fracture_gate_active_ = false;
        }
    }

    const int32_t glide_maximum = 24726;
    const int32_t glide_weight =
        lower_environment == static_cast<uint8_t>(AtmosphereEnvironment::GLIDE) ? lower_weight : 0;
    const int32_t upper_glide_weight =
        upper_environment == static_cast<uint8_t>(AtmosphereEnvironment::GLIDE) ? upper_weight : 0;
    const int32_t glide_amount =
        (static_cast<int64_t>(glide_maximum) * atmosphere_effect_strength_ >> 15) *
        atmosphere_depth_ >> 15;
    const int32_t target_glide = (glide_amount *
        (glide_weight + upper_glide_weight)) >> 15;
    const int32_t glide_delta = target_glide - atmosphere_level_;
    atmosphere_level_ += std::clamp<int32_t>(glide_delta, -32, 32);
    if (std::abs(glide_delta) <= 32) atmosphere_level_ = target_glide;

    ProcessAtmosphereReverbCore(dry);

    const int32_t history = atmosphere_delay_history_;
    const auto tap = [this, history](size_t delay) -> int32_t {
        if (history < static_cast<int32_t>(delay)) return 0;
        size_t position = atmosphere_delay_pos_ + kAtmosphereDelaySamples - delay;
        if (position >= kAtmosphereDelaySamples) position -= kAtmosphereDelaySamples;
        return atmosphere_delay_[position];
    };
    const bool needs_tap70 = lower_environment == static_cast<uint8_t>(AtmosphereEnvironment::ECHO) ||
        upper_environment == static_cast<uint8_t>(AtmosphereEnvironment::ECHO);
    const bool needs_tap140 = needs_tap70;
    const bool needs_tap280 = needs_tap140 ||
        lower_environment == static_cast<uint8_t>(AtmosphereEnvironment::WHISTLE) ||
        upper_environment == static_cast<uint8_t>(AtmosphereEnvironment::WHISTLE);
    const int32_t tap70 = needs_tap70 ? tap(kAtmosphereTap70) : 0;
    const int32_t tap140 = needs_tap140 ? tap(kAtmosphereTap140) : 0;
    const int32_t tap280 = needs_tap280 ? tap(kAtmosphereDelaySamples) : 0;

    int32_t lower_l = 0, lower_r = 0;
    ProcessAtmosphereEnvironment(lower_environment, dry, tap70, tap140, tap280, whistle_tone,
        lower_l, lower_r);
    if (upper_weight > 0)
    {
        int32_t upper_l = 0, upper_r = 0;
        ProcessAtmosphereEnvironment(upper_environment, dry, tap70, tap140, tap280, whistle_tone,
            upper_l, upper_r);
        wet_l = (lower_l * lower_weight + upper_l * upper_weight) >> 15;
        wet_r = (lower_r * lower_weight + upper_r * upper_weight) >> 15;
    }
    else
    {
        wet_l = lower_l;
        wet_r = lower_r;
    }

    // Adjacent-environment weights already crossfade the wet audio before it
    // reaches Presence. Do not smooth the waveform itself here: a per-sample
    // handoff would act as an unintended low-pass filter on every effect.
    wet_l = (wet_l * atmosphere_depth_) >> 15;
    wet_r = (wet_r * atmosphere_depth_) >> 15;

    // At 44 kHz this increment produces a 0.307 Hz (3.26 s) FLANGE sweep.
    atmosphere_lfo_phase_ += 30000u;
    atmosphere_delay_[atmosphere_delay_pos_] = static_cast<int16_t>(dry);
    if (++atmosphere_delay_pos_ == kAtmosphereDelaySamples) atmosphere_delay_pos_ = 0;
    if (atmosphere_delay_history_ < kAtmosphereDelaySamples) ++atmosphere_delay_history_;
}

FASTCODE void AppTemplate::AudioLoop(
    [[maybe_unused]] q15_t *input,
    q15_t *output,
    size_t size)
{
    if (!inited_)
    {
        return;
    }


    distance_pot_->Process();
    tension_pot_->Process();
    atmosphere_depth_pot_->Process();
    voice_a_pot_->Process();
    voice_b_pot_->Process();
    atmosphere_pot_->Process();

    low_pot_->Process();
    rhythm_pot_->Process();
    high_pot_->Process();


    // --------------------------------------------------------
    // PHRASE CLOCK + ORGANIC NOTE SCHEDULER
    // --------------------------------------------------------
    //
    // Bastl's master clock remains rigid.  A grid trigger merely
    // schedules the phrase event.  HOME's remembered timing offset
    // then delays the actual note by a small number of audio samples.
    //
    // This gives Attension timing feel WITHOUT making the master
    // clock itself wobble.

    if (reference_audition_ && audition_settle_samples_ <= 0)
    {
        reference_audition_ = false;
        if (preview_mode_)
        {
            preview_loop_ = preview_resume_loop_;
            env_.SetHold(false);
        }
        saved_journey_ = false;
        phrase_step_ = 0;
    }
    const Clock::Sync clock_sync_type = Kastle2::base.GetClock().GetSyncType();
    const bool clock_running = Kastle2::base.GetClock().GetState() ==
        ClockSource::State::RUNNING;
    if (!clock_running || (clock_timing_running_ &&
                           clock_sync_type != clock_timing_sync_type_))
    {
        InvalidateClockOwnedDelayedEvents();
        samples_since_clock_ = 0;
        last_clock_interval_samples_ = 0;
    }
    clock_timing_running_ = clock_running;
    clock_timing_sync_type_ = clock_sync_type;

    if (Kastle2::base.GetClock().IsNowTrigger())
    {
        // A subdivision belongs to the preceding interval, so an unplayed
        // countdown is stale as soon as this actual edge arrives.
        InvalidateClockOwnedDelayedEvents();

        // Measure the real master-clock period.  Sub-step hat fills use
        // this value, so they follow tempo and external/MIDI sync rather
        // than relying on a fixed millisecond guess.
        if (Kastle2::base.GetClock().IsNowReset())
        {
            samples_since_clock_ = 0;
            last_clock_interval_samples_ = 0;
        }
        else if (samples_since_clock_ > 0)
        {
            last_clock_interval_samples_ =
                samples_since_clock_;
        }

        samples_since_clock_ = 0;

        if (preview_mode_)
        {
            if (preview_rhythm_[preview_step_]) TriggerPreviewClick();
            if (!reference_audition_ && preview_rhythm_[preview_step_])
            {
                preview_pending_pitch_ = preview_pitch_[preview_step_];
                preview_pending_delay_samples_ =
                    preview_timing_samples_[preview_step_] +
                    RhythmFamilyStepDelay(preview_density_, preview_step_);
                preview_pending_note_ = true;
            }
            if (++preview_step_ >= kPhraseLength) preview_step_ = 0;
            phrase_step_ = preview_step_;
        }
        else if (phrase_step_ == 0 && audition_settle_samples_ <= 0)
        {
            if (shift_home_pending_)
            {
                shift_home_pending_ = false;
                shift_audition_ = true;
                RestoreHome();
                StopMelodyAuditionNotes();
                MutePercussionOrnaments();
            }
            else if (shift_release_pending_)
            {
                shift_release_pending_ = false;
                shift_audition_ = false;
                if (saved_journey_)
                {
                    current_rhythm_ = saved_rhythm_;
                    current_pitch_ = saved_pitch_;
                    current_timing_samples_ = saved_timing_;
                    current_distance_ = saved_distance_;
                }
                saved_journey_ = false;
                StopMelodyAuditionNotes();
            }
        }
        // Percussion thinks in phrases.  At step zero it decides
        // whether this phrase deserves a fill, based on WANDER.
        if (!preview_mode_ && !ended_ && phrase_step_ == 0)
        {
            // Do not advance patch-driven compositional targets while a
            // frozen lifecycle/audition path owns the phrase.
            if (!ending_requested_ && !ending_active_ &&
                !ending_cadence_active_ && !shift_audition_ &&
                !shift_home_pending_ && !shift_release_pending_)
                SamplePatchTargetsForPhrase();
            if (ending_cadence_active_ && ending_cadence_bar_ == 0)
            {
                // The HOME reprise has completed. Cancel future melodic work
                // at the first cadence downbeat, then fade the already-audible
                // direct A/B energy while the cadence remains drum-only.
                BeginCadenceMelodyRelease();
            }
            if (!ending_cadence_active_)
            {
                if (!shift_audition_ && !shift_home_pending_ &&
                    !shift_release_pending_)
                {
                    UpdateVoiceBPhraseParticipation();
                    UpdateVoiceBHoldForPhrase();
                }
                const TensionPhase previous_tension_phase = tension_phase_;
                UpdateTensionForPhrase();
                const bool normal_phrase = !ending_requested_ && !ending_active_ &&
                    !shift_audition_ && !shift_home_pending_ &&
                    !shift_release_pending_;
                tension10_chromatic_lock_ = normal_phrase &&
                    tension_amount_ == 10 && (std::rand() % 100) < 55;
                if (tension_phase_ != TensionPhase::RESOLVE)
                {
                    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
                }
                else if ((previous_tension_phase == TensionPhase::BUILD ||
                          previous_tension_phase == TensionPhase::HOLD) &&
                         normal_phrase && !tension10_chromatic_lock_ &&
                         voice_b_lab_selection_ > 0 && !voice_b_lab_off_ &&
                         voice_b_lab_active_selection_ > 0 &&
                         voice_b_has_harmony_note_ && harmony_env_value_ > 0)
                {
                    // A2 only suspends an already-audible B: the first actual
                    // resolving A attack establishes context, then the next
                    // actual A attack may use existing B RESOLVE behavior.
                    voice_b_resolve_suspension_ =
                        VoiceBResolveSuspension::AWAITING_FIRST_A_ATTACK;
                }
                else
                {
                    voice_b_resolve_suspension_ = VoiceBResolveSuspension::NONE;
                }
                UpdateVoiceCharactersForPhrase();
                UpdateAtmosphereForPhrase();
            }
            PlanPercussionPhrase();
            PlanHiHatPhrase();
            PlanTransitionPercussionPhrase();

            const bool my_turn_safe_context =
                !ending_requested_ && !ending_active_ && !ending_cadence_active_ &&
                !shift_audition_ && !shift_home_pending_ && !shift_release_pending_ &&
                tension_phase_ == TensionPhase::STABLE &&
                voice_b_resolve_suspension_ == VoiceBResolveSuspension::NONE;
            const bool my_turn_safe_release = my_turn_safe_context &&
                tension_hold_phrases_ < 0;
            if (my_turn_phase_ != MyTurnPhase::NONE)
            {
                if (my_turn_phase_ == MyTurnPhase::FIRST_PHRASE)
                {
                    my_turn_phase_ = MyTurnPhase::SECOND_PHRASE;
                    MutePercussionOrnaments();
                }
                else if (my_turn_phase_ == MyTurnPhase::SECOND_PHRASE)
                {
                    my_turn_phase_ = MyTurnPhase::THIRD_PHRASE;
                    MutePercussionOrnaments();
                }
                else if (my_turn_phase_ == MyTurnPhase::THIRD_PHRASE)
                {
                    my_turn_phase_ = MyTurnPhase::FOURTH_PHRASE;
                    MutePercussionOrnaments();
                }
                else
                {
                    ResetMyTurnPerformance();
                }
            }
            else if (my_turn_safe_release &&
                     !home_refrain_active_ &&
                     my_turn_normal_phrases_ >= my_turn_arm_phrases_)
            {
                // Begin only on a clean, stable phrase once its previously
                // drawn ordinary-phrase interval has elapsed.
                my_turn_phase_ = MyTurnPhase::FIRST_PHRASE;
                MutePercussionOrnaments();
            }
            UpdateCymbalRestraintForPhrase();
        }

        if (!preview_mode_ && !ended_)
        {
            TriggerPercussionForStep(phrase_step_);
            if (!shift_audition_ && my_turn_phase_ == MyTurnPhase::NONE)
            {
                TriggerHiHatForStep(phrase_step_);
            }
        }


        if (!preview_mode_ && !ended_ && !ending_cadence_active_ &&
            !reference_audition_ && audition_settle_samples_ <= 0)
        {
            // A sudden clock acceleration may reach step zero before
            // its pickup. Sound that pickup first, then leave a brief
            // gap before step zero rather than overwriting it.
            if (pending_note_ && pending_pickup_)
            {
                TriggerPendingNote();
                pickup_recovery_samples_ = std::min(
                    MillisecondsToSamples(12),
                    last_clock_interval_samples_ / 4);
            }
            pending_note_ = false;
            pending_note_is_resolve_ = false;
            pending_pickup_ = false;
            const bool home_presentation = shift_audition_ || ending_active_ ||
                home_refrain_active_;
            const bool phrase_attack = home_presentation ?
                home_rhythm_[phrase_step_] : current_rhythm_[phrase_step_];
            if (phrase_attack)
            {
                SchedulePhraseNote(phrase_step_, home_presentation,
                    shift_audition_ || ending_active_);
            }
            phrase_step_++;
        }
        else if (!preview_mode_ && !ended_ && ending_cadence_active_)
        {
            // The cadence is drum-only, but still advances through one
            // complete phrase so its last kick+snare can end decisively.
            phrase_step_++;
        }

        if (!preview_mode_ && phrase_step_ >=
            kPhraseLength)
        {
            phrase_step_ = 0;

            if (ending_requested_)
            {
                ending_requested_ = false;
                ending_active_ = true;
                ending_home_phrases_remaining_ = 2;
                if (voice_b_arrangement_ == VoiceBArrangement::HOLD)
                    EnterVoiceBRest();
                RestoreHome();
            }
            else if (ending_active_)
            {
                if (ending_home_phrases_remaining_ > 0)
                    --ending_home_phrases_remaining_;
                if (ending_home_phrases_remaining_ == 0)
                {
                    ending_active_ = false;
                    ending_cadence_active_ = true;
                    ending_cadence_bar_ = 0;
                    // The dedicated cadence begins cleanly: no delayed/open hat
                    // or residual hat envelope may cross its downbeat.
                    transition_open_hat_pending_ = false;
                    hihat_subhit_1_samples_ = hihat_subhit_2_samples_ =
                        hihat_subhit_3_samples_ = -1;
                    hihat_env_.Reset();
                }
            }
            else if (ending_cadence_active_)
            {
                if (++ending_cadence_bar_ >= ending_cadence_bars_)
                    ending_finalizing_ = true;
            }
            else if (!shift_audition_ && !shift_home_pending_ &&
                     !shift_release_pending_)
            {
                if (my_turn_phase_ == MyTurnPhase::NONE && !reference_audition_ &&
                    audition_settle_samples_ <= 0 &&
                    my_turn_normal_phrases_ < my_turn_arm_phrases_)
                    ++my_turn_normal_phrases_;
                ProcessDistanceJourney();
                UpdateVoiceARefrainPresentation();
            }
        }
    }


    // --------------------------------------------------------
    // AUDIO
    // --------------------------------------------------------

    // DREAM pitch travel advances once per audio block. This keeps the
    // oscillator's period division out of the per-sample path.
    if (!reference_audition_)
        UpdateDreamGlissando();

    for (size_t i = 0;
         i < size;
         i++)
    {
        const q15_t my_turn_target = my_turn_phase_ == MyTurnPhase::NONE ?
            Q15_MAX : kMyTurnMelodyGain;
        if (my_turn_melody_gain_ < my_turn_target)
            my_turn_melody_gain_ = static_cast<q15_t>(std::min<int32_t>(
                my_turn_target, my_turn_melody_gain_ + kMyTurnFadeInStep));
        else if (my_turn_melody_gain_ > my_turn_target)
            my_turn_melody_gain_ = static_cast<q15_t>(std::max<int32_t>(
                my_turn_target, my_turn_melody_gain_ - kMyTurnFadeOutStep));

        const bool break_it_down_b_suppressed = shift_home_pending_ ||
            ((shift_audition_ || shift_release_pending_) &&
             !reference_audition_);
        const q15_t shift_voice_b_target = break_it_down_b_suppressed ?
            0 : Q15_MAX;
        if (shift_voice_b_presentation_gain_ < shift_voice_b_target)
            shift_voice_b_presentation_gain_ = static_cast<q15_t>(
                std::min<int32_t>(shift_voice_b_target,
                    shift_voice_b_presentation_gain_ +
                    kShiftVoiceBPresentationStep));
        else if (shift_voice_b_presentation_gain_ > shift_voice_b_target)
            shift_voice_b_presentation_gain_ = static_cast<q15_t>(
                std::max<int32_t>(shift_voice_b_target,
                    shift_voice_b_presentation_gain_ -
                    kShiftVoiceBPresentationStep));

        samples_since_clock_++;
        if (audition_settle_samples_ > 0) --audition_settle_samples_;

        ProcessHiHatSubStepScheduler();

        if (pending_note_)
        {
            if (pending_note_delay_samples_ <= 0)
            {
                TriggerPendingNote();
            }
            else
            {
                pending_note_delay_samples_--;
            }
        }


        q15_t melody_out =
            q31_to_q15(osc_.Process());
        q15_t voice_a_layer_out =
            q31_to_q15(voice_a_layer_osc_.Process());
        q15_t voice_a_fm_out = Q15_ZERO;
        if (voice_a_uses_fm_)
            voice_a_fm_out = q31_to_q15(voice_a_fm_.Process());


        env_value_ =
            q31_to_q15(
                env_.Process());


        melody_out = q15_mult(melody_out, env_value_);
        const q15_t reference_sine_out = melody_out;
        voice_a_layer_out = q15_mult(voice_a_layer_out, env_value_);
        voice_a_fm_out = q15_mult(voice_a_fm_out, env_value_);

        int32_t melody_mix;
        if (active_voice_a_ == VoiceCharacter::ODDITY)
        {
            melody_mix = q15_mult(melody_out, q15(0.72f)) +
                q15_mult(voice_a_layer_out, q15(0.28f));
            if (voice_a_oddity_glint_delay_ > 0)
            {
                --voice_a_oddity_glint_delay_;
            }
            else if (voice_a_oddity_glint_remaining_ > 0)
            {
                constexpr q31_t kGlintRampIncrement = 67108863;
                constexpr q31_t kGlintDecayDecrement = 951899;
                if (voice_a_oddity_glint_ramp_ < 32)
                {
                    voice_a_oddity_glint_envelope_ = std::min<q31_t>(
                        Q31_MAX, voice_a_oddity_glint_envelope_ + kGlintRampIncrement);
                    ++voice_a_oddity_glint_ramp_;
                }
                else
                {
                    voice_a_oddity_glint_envelope_ = std::max<q31_t>(
                        0, voice_a_oddity_glint_envelope_ - kGlintDecayDecrement);
                }
                const q15_t level = q15_mult(q15(0.11f),
                    q31_to_q15(voice_a_oddity_glint_envelope_));
                melody_mix += q15_mult(
                    q31_to_q15(voice_a_oddity_glint_.Process()), level);
                --voice_a_oddity_glint_remaining_;
            }
        }
        else if (voice_a_uses_fm_)
        {
            melody_mix = voice_a_fm_out;
        }
        else
        {
            melody_mix = static_cast<int32_t>(melody_out) +
                 q15_mult(voice_a_layer_out, voice_a_layer_level_);
        }
        melody_mix = std::clamp<int32_t>(melody_mix, -32768, 32767);
        melody_out = q15_mult(static_cast<q15_t>(melody_mix),
            voice_a_output_level_);


        q15_t harmony_out = Q15_ZERO;


        harmony_env_value_ =
            q31_to_q15(
                harmony_env_.Process());


        if (voice_b_lab_active_selection_ > 0)
        {
            harmony_out = ProcessVoiceBLab(harmony_env_value_);
        }

        if (cadence_melody_release_samples_ > 0)
        {
            const q15_t cadence_gain = static_cast<q15_t>(
                (static_cast<uint32_t>(Q15_MAX) * cadence_melody_release_samples_) /
                kCadenceMelodyReleaseSamples);
            melody_out = q15_mult(melody_out, cadence_gain);
            harmony_out = q15_mult(harmony_out, cadence_gain);

            if (--cadence_melody_release_samples_ == 0)
            {
                // At silence, retire the envelope and any active glint state.
                StopMelodyAuditionNotes();
            }
        }


        q15_t percussion_out =
            q31_to_q15(
                percussion_osc_.Process());


        percussion_env_value_ =
            q31_to_q15(
                percussion_env_.Process());


        percussion_out =
            q15_mult(
                percussion_out,
                percussion_env_value_);

        int32_t rhythm_detent_out = 0;
        if (rhythm_detent_body_remaining_ > 0 ||
            rhythm_detent_glint_delay_ > 0 ||
            rhythm_detent_glint_remaining_ > 0)
        {
            constexpr uint16_t kBodySamples = SAMPLE_RATE * 38 / 1000;
            constexpr uint16_t kBodyAttackSamples = SAMPLE_RATE / 1000;
            constexpr uint16_t kGlintSamples = SAMPLE_RATE * 27 / 1000;
            if (rhythm_detent_body_remaining_ > 0)
            {
                const uint16_t age = kBodySamples - rhythm_detent_body_remaining_;
                const q15_t envelope = age < kBodyAttackSamples ?
                    static_cast<q15_t>((static_cast<int32_t>(age) * Q15_MAX) /
                                       kBodyAttackSamples) :
                    static_cast<q15_t>((static_cast<int32_t>(rhythm_detent_body_remaining_) *
                                        Q15_MAX) / (kBodySamples - kBodyAttackSamples));
                rhythm_detent_out = q15_mult(
                    q31_to_q15(rhythm_detent_body_osc_.Process()), envelope);
                --rhythm_detent_body_remaining_;
            }
            if (rhythm_detent_glint_delay_ > 0)
            {
                --rhythm_detent_glint_delay_;
            }
            else if (rhythm_detent_glint_remaining_ > 0)
            {
                const q15_t envelope = static_cast<q15_t>(
                    (static_cast<int32_t>(rhythm_detent_glint_remaining_) *
                     q15(0.11f)) / kGlintSamples);
                rhythm_detent_out += q15_mult(
                    q31_to_q15(rhythm_detent_glint_osc_.Process()), envelope);
                --rhythm_detent_glint_remaining_;
            }
            rhythm_detent_out = q15_mult(static_cast<q15_t>(
                std::clamp<int32_t>(rhythm_detent_out, -32768, 32767)), q15(0.075f));
        }

        if (preview_pending_note_)
        {
            if (preview_pending_delay_samples_ <= 0)
            {
                TriggerPreviewSine(preview_pending_pitch_);
                preview_pending_note_ = false;
            }
            else
            {
                preview_pending_delay_samples_--;
            }
        }


        // ----------------------------------------------------
        // HI-HAT NOISE
        // ----------------------------------------------------
        //
        // xorshift32: cheap enough for the audio loop and deterministic.
        // The envelope turns the broadband noise into a short "tick".

        hihat_noise_state_ ^=
            hihat_noise_state_ << 13;

        hihat_noise_state_ ^=
            hihat_noise_state_ >> 17;

        hihat_noise_state_ ^=
            hihat_noise_state_ << 5;

        const q15_t percussion_noise =
            static_cast<q15_t>(
                static_cast<int16_t>(
                    hihat_noise_state_ >> 16));

        q15_t hihat_out = percussion_noise;


        hihat_env_value_ =
            q31_to_q15(
                hihat_env_.Process());


        hihat_out =
            q15_mult(
                hihat_out,
                hihat_env_value_);

        const q15_t snare_env = q31_to_q15(snare_env_.Process());
        const q15_t snare_out = q15_mult(percussion_noise, snare_env);

        // HOME is also protected at the mixer itself.
        //
        // Even if Voice B still has envelope energy left from the
        // journey inward, it cannot leak into exact HOME. At HOME
        // Voice A returns to the original single-voice /4 level.
        // Away from HOME, both voices retain the established /8 +
        // /8 headroom.

        // --------------------------------------------------------
        // GAIN STAGING
        // --------------------------------------------------------
        //
        // Voice A carries Attension's remembered melodic identity,
        // so its level stays constant throughout the journey.
        // Voice B joins underneath it instead of forcing A quieter.
        //
        // HOME still hard-mutes Voice B. Away from HOME, B is mixed
        // at half A's amplitude. The final sum is clamped to q15
        // range as a last-resort safety guard.

        int32_t mixed;
        int32_t wet_l = 0;
        int32_t wet_r = 0;
        if (preview_mode_)
        {
            mixed = static_cast<int32_t>(percussion_out / 6) +
                static_cast<int32_t>(reference_sine_out / 4);
        }
        else if (reference_audition_)
        {
            // LOW/HIGH audition is a fixed, dry SOFT-voice reference tone.
            mixed = static_cast<int32_t>(reference_sine_out / 4);
        }
        else
        {
            const bool shift_home_presentation = shift_audition_ &&
                !reference_audition_ && voice_a_lab_off_;
            mixed = (voice_a_lab_off_ && !shift_home_presentation) ? 0 :
                static_cast<int32_t>(melody_out / 4);

            int32_t harmony_mix = 0;
            if (voice_b_lab_selection_ > 0)
            {
                harmony_mix = static_cast<int32_t>(harmony_out / 8);
                mixed += harmony_mix;
            }

            // Preserve existing ATMOS/ECHO tails, but do not feed them the
            // direct cadence-release material.
            ProcessAtmosphere(ending_cadence_active_ ? 0 : mixed, wet_l, wet_r);

            // FRACTURE gates A+B only; percussion is added afterwards.
            const int32_t gate_loss = Q15_MAX - atmosphere_fracture_gate_level_;
            const int32_t fracture_authority =
                (static_cast<int32_t>(atmosphere_fracture_weight_) * atmosphere_depth_) >> 15;
            const int32_t effective_gate = Q15_MAX -
                ((fracture_authority * gate_loss) >> 15);
            mixed = (mixed * effective_gate) >> 15;

            // Preserve B's ordinary shared Atmosphere input, but gently
            // attenuate only its listener-facing direct contribution while
            // BREAK IT DOWN is pending, active, or awaiting restoration.
            if (shift_voice_b_presentation_gain_ != Q15_MAX && harmony_mix != 0)
            {
                const int32_t direct_harmony =
                    (harmony_mix * effective_gate) >> 15;
                mixed -= direct_harmony;
                mixed += (direct_harmony * shift_voice_b_presentation_gain_) >> 15;
            }

            // MY TURN leaves the complete A/B + processed ATMOS signal alive,
            // then gives its two authored drum phrases the foreground. The wet
            // and dry portions share one smooth 50% presentation gain.
            if (my_turn_melody_gain_ != Q15_MAX)
            {
                mixed = static_cast<int32_t>((static_cast<int64_t>(mixed) *
                    my_turn_melody_gain_) >> 15);
                wet_l = static_cast<int32_t>((static_cast<int64_t>(wet_l) *
                    my_turn_melody_gain_) >> 15);
                wet_r = static_cast<int32_t>((static_cast<int64_t>(wet_r) *
                    my_turn_melody_gain_) >> 15);
            }

            // My Turn brings the already-scaled kick, hat, and snare mix one
            // quarter forward at the final presentation seam. Normal drum
            // presentation remains exactly on its established path.
            if (my_turn_phase_ != MyTurnPhase::NONE)
            {
                const int32_t drum_mix =
                    std::clamp<int32_t>(percussion_out * 5 / 24, -32768, 32767) +
                    hihat_out / 12 + snare_out / 10;
                mixed += drum_mix * 5 / 4;
            }
            else
            {
                // Keep the bass thump audible beneath melodic voices while
                // preserving conservative headroom.
                mixed += std::clamp<int32_t>(percussion_out * 5 / 24, -32768, 32767);

                // Hi-hat stays lighter than the thump. Its job is to make the
                // grid audible ("tick tick tick tick"), not dominate the mix.
                mixed += hihat_out / 12;
                mixed += snare_out / 10;
            }

        }

        output[2 * i] = static_cast<q15_t>(
            std::clamp<int32_t>(mixed + wet_l + rhythm_detent_out, -32768, 32767));
        output[2 * i + 1] = static_cast<q15_t>(
            std::clamp<int32_t>(mixed + wet_r + rhythm_detent_out, -32768, 32767));
    }

    if (ending_finalizing_)
    {
        ending_finalizing_ = false;
        ending_cadence_active_ = false;
        ended_ = true;
        last_voice_b_midi_note_ = -1;
        // Keep the authored final kick and snare decay alive after their
        // final cadence trigger; only future melodic and hat work is stopped.
        ClearFlourishState();
        StopMelodyAuditionNotes();
        hihat_subhit_1_samples_ = hihat_subhit_2_samples_ =
            hihat_subhit_3_samples_ = -1;
        hihat_env_.Reset();
    }
}


// ------------------------------------------------------------
// USER INTERFACE
// ------------------------------------------------------------

uint32_t AppTemplate::InterpolateLedColor(const uint32_t *anchors,
                                          size_t anchor_count, int32_t position)
{
    position = std::clamp<int32_t>(position, 0, Q15_MAX);
    const int32_t scaled = position * static_cast<int32_t>(anchor_count - 1);
    const size_t lower = std::min(anchor_count - 1,
        static_cast<size_t>(scaled / Q15_MAX));
    const size_t upper = std::min(anchor_count - 1, lower + 1);
    const int32_t fraction = lower == upper ? 0 : scaled - lower * Q15_MAX;
    const uint32_t a = anchors[lower];
    const uint32_t b = anchors[upper];
    const auto mix = [fraction](uint8_t left, uint8_t right) -> uint8_t {
        return static_cast<uint8_t>(left +
            (static_cast<int32_t>(right - left) * fraction) / Q15_MAX);
    };
    return WS2812::RGB(mix(a >> 16, b >> 16), mix(a >> 8, b >> 8), mix(a, b));
}

bool AppTemplate::UpdateFoxVisit(uint32_t now_ms, bool start_blocked)
{
    // UI-side only: its private stream never touches musical composition RNG.
    const auto next_interval_ms = [this]() -> uint32_t {
        fox_visit_rng_ ^= fox_visit_rng_ << 13;
        fox_visit_rng_ ^= fox_visit_rng_ >> 17;
        fox_visit_rng_ ^= fox_visit_rng_ << 5;
        return (900u + fox_visit_rng_ % 601u) * 1000u;
    };
    const auto deadline_reached = [now_ms](uint32_t deadline) {
        return static_cast<int32_t>(now_ms - deadline) >= 0;
    };
    if (!fox_visit_timer_initialized_)
    {
        fox_visit_timer_initialized_ = true;
        fox_visit_next_ms_ = now_ms + next_interval_ms();
    }
    // Preview blocks only a new start. Once underway, Fox Visit owns its LED
    // story through SHIFT, MODE, Preview, ending, and idle playback states.
    if (!fox_visit_active_ && !start_blocked &&
        deadline_reached(fox_visit_next_ms_))
    {
        fox_visit_active_ = true;
        fox_visit_started_ms_ = now_ms;
    }
    if (!fox_visit_active_) return false;

    struct Stage { uint16_t duration, rise, dwell; int8_t hole; uint8_t kind, peak; };
    static constexpr Stage opening[] = {
        {2300, 0, 0, -1, 1, 0}, {900, 0, 0, -1, 0, 0},
        {350, 100, 120, 0, 2, 13}, {300, 0, 0, -1, 0, 0},
        {2800, 1100, 650, 0, 2, 26}, {1700, 0, 0, -1, 0, 0},
        {2200, 650, 750, 1, 2, 102}, {1100, 0, 0, -1, 0, 0},
        {1800, 250, 800, 2, 2, 191}, {2200, 0, 0, -1, 0, 0},
    };
    struct RunPass { uint16_t rise, dwell, retreat, travel; };
    static constexpr RunPass circular_passes[] = {
        // Complete LED1 -> LED2 -> LED3 circuits.  Confidence arrives early;
        // the final zoomies remain quick but perceptibly separated.
        {90, 420, 140, 183}, {70, 350, 110, 87}, {55, 270, 85, 57},
        {45, 205, 65, 35}, {35, 150, 50, 25}, {25, 110, 35, 20},
        {18, 85, 27, 20}, {14, 68, 22, 16}, {12, 58, 18, 12},
    };
    static constexpr Stage ending[] = {
        {430, 60, 220, 2, 2, 255}, {500, 0, 0, -1, 0, 0},
        {2100, 80, 1400, 0, 2, 255}, {1200, 0, 0, -1, 0, 0},
        {2300, 80, 1400, 1, 2, 255}, {360, 0, 0, -1, 0, 0},
        {110, 1, 108, 1, 2, 255}, {45, 0, 0, -1, 0, 0},
        {70, 1, 68, 1, 2, 255},
        {1200, 0, 0, -1, 0, 0}, {2300, 0, 0, -1, 1, 0},
    };
    uint32_t elapsed = now_ms - fox_visit_started_ms_;
    Stage s{};
    const auto select_stage = [&elapsed, &s](const Stage &candidate) {
        if (elapsed < candidate.duration)
        {
            s = candidate;
            return true;
        }
        elapsed -= candidate.duration;
        return false;
    };
    bool stage_selected = false;
    for (const Stage &candidate : opening)
    {
        if (select_stage(candidate))
        {
            stage_selected = true;
            break;
        }
    }
    // Each pass follows the clockwise tunnel route LED1 -> LED2 -> LED3.
    if (!stage_selected) for (size_t pass = 0; pass < std::size(circular_passes); ++pass)
    {
        const RunPass &timing = circular_passes[pass];
        const uint16_t duration = timing.rise + timing.dwell + timing.retreat;
        for (int hole = 0; hole < 3; ++hole)
        {
            if (select_stage({duration, timing.rise, timing.dwell,
                              static_cast<int8_t>(hole), 2, 255}))
            {
                stage_selected = true;
                break;
            }
            if (select_stage({timing.travel, 0, 0, -1, 0, 0}))
            {
                stage_selected = true;
                break;
            }
        }
        if (stage_selected) break;
    }
    // The 550 ms comic stop follows, rather than replaces, circuit nine's
    // normal 210 ms timing.  ending[0] is the distinct surprise LED3 repeat.
    if (!stage_selected && select_stage({550, 0, 0, -1, 0, 0}))
        stage_selected = true;
    if (!stage_selected) for (const Stage &candidate : ending)
    {
        if (select_stage(candidate))
        {
            stage_selected = true;
            break;
        }
    }
    if (!stage_selected)
    {
        fox_visit_active_ = false;
        fox_visit_next_ms_ = now_ms + next_interval_ms();
        return false;
    }
    const uint16_t phase = static_cast<uint16_t>((elapsed * 255u) / s.duration);
    const uint8_t bloom = static_cast<uint8_t>((phase < 128 ? phase : 255 - phase) * 2);
    const uint32_t fox = WS2812::RGB(255, 104, 18);
    static constexpr Hardware::Led leds[] = {
        Hardware::Led::LED_1, Hardware::Led::LED_2, Hardware::Led::LED_3};
    static const uint32_t fanfare[] = {
        WS2812::RGB(8, 180, 165), WS2812::RGB(105, 45, 220),
        WS2812::RGB(255, 170, 20), WS2812::RGB(240, 35, 135),
        WS2812::RGB(255, 210, 105)};
    uint8_t fox_brightness = 0;
    if (s.kind == 2)
    {
        const uint16_t retreat = s.duration - s.rise - s.dwell;
        if (elapsed < s.rise)
            fox_brightness = static_cast<uint8_t>((elapsed * s.peak) / s.rise);
        else if (elapsed < static_cast<uint32_t>(s.rise) + s.dwell)
            fox_brightness = s.peak;
        else
            fox_brightness = static_cast<uint8_t>(
                ((s.duration - elapsed) * s.peak) / retreat);
    }
    const uint8_t fanfare_brightness = static_cast<uint8_t>(
        32 + (static_cast<uint16_t>(bloom) * 208) / 255);
    const uint32_t fanfare_color = InterpolateLedColor(fanfare, std::size(fanfare),
        static_cast<int32_t>(phase) * Q15_MAX / 255);
    for (int i = 0; i < 3; ++i)
    {
        uint32_t color = WS2812::NONE;
        if (s.kind == 1) color = WS2812::ApplyBrightness(fanfare_color, fanfare_brightness);
        else if (s.kind == 2 && i == s.hole) color = WS2812::ApplyBrightness(fox, fox_brightness);
        Kastle2::hw.SetLed(leds[i], color);
    }
    return true;
}

void AppTemplate::UpdateAttensionLeds(bool shift_pressed)
{
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (UpdateFoxVisit(now_ms, preview_mode_ || shift_pressed)) return;
    if (preview_mode_)
    {
        Kastle2::hw.SetLed(Hardware::Led::LED_1, WS2812::NONE);
        Kastle2::hw.SetLed(Hardware::Led::LED_2, WS2812::NONE);
        Kastle2::hw.SetLed(Hardware::Led::LED_3, WS2812::GREEN);
        return;
    }

    // Sound: these anchors follow the exact seven ATMOS environments.
    static const std::array<uint32_t, 7> kAtmosColors = {
        WS2812::RGB(255, 28, 20), WS2812::RGB(255, 105, 16),
        WS2812::RGB(255, 210, 20), WS2812::RGB(32, 205, 80),
        WS2812::RGB(25, 100, 255), WS2812::RGB(80, 48, 180),
        WS2812::RGB(190, 50, 220)};
    const uint8_t tension_brightness = static_cast<uint8_t>(
        kLedTensionMinimumBrightness +
        (static_cast<int32_t>(kLedTensionMaximumBrightness -
            kLedTensionMinimumBrightness) *
            std::clamp<int32_t>(tension_cv_value_, POT_MIN, POT_MAX)) /
            (POT_MAX - POT_MIN));
    const uint8_t presence_brightness = static_cast<uint8_t>(
        kLedPresenceMinimumBrightness +
        (((kLedTensionMaximumBrightness - kLedPresenceMinimumBrightness) *
          atmosphere_depth_ + Q15_MAX / 2) / Q15_MAX));
    Kastle2::hw.SetLed(Hardware::Led::LED_1, WS2812::ApplyBrightness(
        InterpolateLedColor(kAtmosColors.data(), kAtmosColors.size(), atmosphere_position_),
        presence_brightness));

    // Journey control is a continuous cool-to-hot reverse spectrum.
    static const std::array<uint32_t, 8> kWanderColors = {
        WS2812::RGB(148, 0, 211), WS2812::RGB(55, 20, 190),
        WS2812::RGB(0, 80, 255), WS2812::RGB(0, 210, 230),
        WS2812::RGB(0, 200, 55),
        WS2812::RGB(245, 220, 0), WS2812::RGB(255, 110, 0),
        WS2812::RGB(255, 20, 0)};
    const int32_t displayed_wander_value = ending_led_frozen_ && !ended_ ?
        ending_led_wander_value_ : wander_cv_value_;
    const int32_t wander_position = static_cast<int32_t>(
        (static_cast<int64_t>(displayed_wander_value - POT_MIN) * Q15_MAX) /
        (POT_MAX - POT_MIN));
    const uint8_t frozen_tension_brightness = static_cast<uint8_t>(
        kLedTensionMinimumBrightness +
        ((kLedTensionMaximumBrightness - kLedTensionMinimumBrightness) * ending_led_tension_) /
            (kTensionPositions - 1));
    const uint8_t wander_brightness = ended_ ? 0 :
        (ending_led_frozen_ ? frozen_tension_brightness : tension_brightness);
    Kastle2::hw.SetLed(Hardware::Led::LED_2, WS2812::ApplyBrightness(
        InterpolateLedColor(kWanderColors.data(), kWanderColors.size(), wander_position),
        wander_brightness));

    if (now_ms < preview_led_until_ms_)
        Kastle2::hw.SetLed(Hardware::Led::LED_3, WS2812::WHITE);
    else if (shift_pressed)
    {
        const bool pulse = Kastle2::base.GetFakeBlinker().GetTempoLedState(
            Kastle2::base.GetClock());
        Kastle2::hw.SetLed(Hardware::Led::LED_3, pulse ? WS2812::CYAN : WS2812::NONE);
    }
    else
        Kastle2::hw.SetLed(Hardware::Led::LED_3, WS2812::NONE);
}

void AppTemplate::UiLoop()
{
    distance_pot_->ReadValue();
    tension_pot_->ReadValue();
    atmosphere_depth_pot_->ReadValue();
    voice_a_pot_->ReadValue();
    voice_b_pot_->ReadValue();
    atmosphere_pot_->ReadValue();

    low_pot_->ReadValue();
    rhythm_pot_->ReadValue();
    high_pot_->ReadValue();


    // --------------------------------------------------------
    // PERFORMANCE
    // --------------------------------------------------------

    int new_requested_distance = distance_pot_->GetMappedValue();

    int new_tension_amount =
        tension_pot_->
            GetMappedValue();

    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    const int32_t raw_atmos = atmosphere_pot_->GetValue();
    const int32_t raw_presence = atmosphere_depth_pot_->GetValue();
    const int32_t raw_tension = tension_pot_->GetValue();
    const int32_t raw_wander = distance_pot_->GetValue();
    const int32_t free_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::PITCH_1);
    const int32_t step_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::PITCH_2);
    const int32_t presence_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::PARAM_1);
    const int32_t tension_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::PARAM_2);
    const int32_t wander_param3_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::PARAM_3);
    const int32_t wander_cv = Kastle2::hw.GetAnalogValue(Hardware::AnalogInput::TRIG_IN);

    // Deliberate physical motion immediately returns ownership to the player.
    // Releasing the detector at the current input sample means a parked cable
    // cannot take the destination back until it moves again after this gesture.
    if (DetectManualPatchTakeover(raw_atmos, previous_manual_atmos_))
    {
        ReleasePatchMod(free_patch_, free_cv, raw_atmos, now_ms);
        ReleasePatchMod(step_patch_, step_cv, raw_atmos, now_ms);
        free_atmos_value_ = raw_atmos;
        step_atmos_value_ = raw_atmos;
    }
    if (DetectManualPatchTakeover(raw_presence, previous_manual_presence_))
    {
        ReleasePatchMod(presence_patch_, presence_cv, raw_presence, now_ms);
        presence_value_ = raw_presence;
    }
    if (DetectManualPatchTakeover(raw_tension, previous_manual_tension_raw_))
    {
        ReleasePatchMod(tension_patch_, tension_cv, raw_tension, now_ms);
        tension_cv_value_ = raw_tension;
    }
    if (DetectManualPatchTakeover(raw_wander, previous_manual_wander_raw_))
    {
        ReleasePatchMod(wander_patch_, wander_cv, raw_wander, now_ms);
        ReleasePatchMod(wander_param3_patch_, wander_param3_cv, raw_wander, now_ms);
        wander_cv_value_ = raw_wander;
    }

    free_atmos_value_ = UpdatePatchMod(free_patch_,
        free_cv, raw_atmos, now_ms, true);
    const bool step_was_active = step_patch_.active;
    int32_t step_value = UpdatePatchMod(step_patch_,
        step_cv, raw_atmos, now_ms, false);
    if (step_was_active && !step_patch_.active &&
        step_patch_.recenter_started_ms != 0)
    {
        // STEP held this value for the phrase; return from what the listener
        // actually heard, not from later unseen input tracking.
        step_patch_.recenter_from = step_atmos_value_;
        step_value = step_atmos_value_;
    }
    if (!step_patch_.active) step_atmos_value_ = step_value;
    presence_value_ = UpdatePatchMod(presence_patch_,
        presence_cv, raw_presence, now_ms, true);
    tension_cv_value_ = UpdatePatchMod(tension_patch_,
        tension_cv, raw_tension, now_ms, false);
    const int32_t trig_wander_value = UpdatePatchMod(wander_patch_,
        wander_cv, raw_wander, now_ms, false);
    const int32_t param3_wander_value = UpdatePatchMod(wander_param3_patch_,
        wander_param3_cv, raw_wander, now_ms, false);
    wander_cv_value_ = (wander_patch_.active || !wander_param3_patch_.active) ?
        trig_wander_value : param3_wander_value;

    const bool initialize_voice_dials = !voice_dials_initialized_ &&
        Kastle2::base.GetLayerTimer() > FancyPot::DEFAULT_LAYER_TIME;
    const int previous_voice_a_selection = voice_a_lab_off_ ? 0 :
        static_cast<int>(selected_voice_a_) + 1;
    const int voice_a_selection = MapVoiceDialSelection(
        voice_a_pot_->GetValue(), previous_voice_a_selection,
        !initialize_voice_dials);
    const bool new_voice_a_lab_off = (voice_a_selection == 0);
    VoiceCharacter new_voice_a = new_voice_a_lab_off ? selected_voice_a_ :
        static_cast<VoiceCharacter>(voice_a_selection - 1);

    if (!preview_mode_)
    {
        requested_tension_ = tension_patch_.active ?
            MapControlToPosition(tension_cv_value_, kTensionPositions) :
            new_tension_amount;

        voice_a_lab_off_ = new_voice_a_lab_off;
        const int new_voice_b_selection = MapVoiceDialSelection(
            voice_b_pot_->GetValue(), voice_b_lab_selection_,
            !initialize_voice_dials);
        if (voice_b_lab_selection_ > 0 && new_voice_b_selection == 0)
        {
            EnterVoiceBRest();
            last_voice_b_midi_note_ = -1;
        }
        voice_b_lab_selection_ = new_voice_b_selection;
        if (new_voice_a != selected_voice_a_)
        {
            selected_voice_a_ = new_voice_a;
            temporary_voice_a_ = false;
        }
        if (initialize_voice_dials)
            voice_dials_initialized_ = true;
    }

    // ATMOS is a continuous seven-environment axis. The audio path smooths
    // this normalized target and blends only adjacent environment centres.
    if (!preview_mode_)
    {
        // FREE and STEP both express bipolar movement about ATMOS.  STEP is
        // held by SamplePatchTargetsForPhrase; FREE remains continuously live.
        const int32_t atmosphere_value = std::clamp<int32_t>(raw_atmos +
            (free_atmos_value_ - raw_atmos) + (step_atmos_value_ - raw_atmos),
            POT_MIN, POT_MAX);
        // Hardware pots can stop a few ADC counts short of their mathematical
        // maximum.  Reserve only that final sliver for the pure GLIDE endpoint.
        constexpr int32_t kGlideEndpointDeadband = 16;
        atmosphere_position_target_ = atmosphere_value >= POT_MAX -
                kGlideEndpointDeadband ? Q15_MAX : static_cast<q15_t>(
            (static_cast<int64_t>(atmosphere_value - POT_MIN) * Q15_MAX) /
            (POT_MAX - POT_MIN));

        const int new_depth = std::clamp<int>(
            MapControlToPosition(presence_value_, 256), 0, 255);
        atmosphere_depth_ = static_cast<q15_t>(new_depth * Q15_MAX / 255);
        atmosphere_effect_strength_ = Q15_MAX;
        if (new_tension_amount != previous_manual_tension_)
        {
            previous_manual_tension_ = new_tension_amount;
            tension_chased_ = new_tension_amount;

            // The phrase planner adopts this discrete request at the next
            // normal phrase boundary; the current phrase keeps its setting.
        }
    }


    // A deliberate user movement immediately invalidates the
    // current parked/living agenda.
    //
    // The journey engine gets priority again.

    if (!preview_mode_ && new_requested_distance != previous_manual_wander_)
    {
        previous_manual_wander_ = new_requested_distance;
        wander_chased_ = new_requested_distance;
        effective_wander_ = wander_chased_;
        requested_distance_ =
            new_requested_distance;

        ResetLivingState();
    }


    const bool shift_pressed = Kastle2::hw.Pressed(Hardware::Button::SHIFT);
    if (suppress_shift_until_release_ && !shift_pressed)
        suppress_shift_until_release_ = false;
    if (!preview_mode_ && !shift_pressed &&
        Kastle2::hw.JustPressed(Hardware::Button::MODE))
    {
        if (ended_)
        {
            RestartEndedPlayback();
            ended_ = false;
            ending_active_ = false;
            ending_cadence_active_ = false;
            RestoreHome();
            phrase_step_ = 0;
        }
        else
        {
            CancelMyTurn();
            ending_led_frozen_ = true;
            ending_led_wander_value_ = wander_cv_value_;
            ending_led_tension_ = static_cast<uint8_t>(adopted_tension_);
            ResetVoiceARefrainState();
            ending_requested_ = true;
            // Ending eligibility belongs to the gesture, not the HOME phrase
            // that will be replayed after it.
            SelectEndingPlan();
            ClearFlourishState();
            transition_open_hat_pending_ = false;
            hihat_subhit_1_samples_ = hihat_subhit_2_samples_ =
                hihat_subhit_3_samples_ = -1;
        }
    }
    if (preview_mode_)
    {
        if (Kastle2::hw.JustPressed(Hardware::Button::SHIFT))
        {
            CommitPreviewMode();
        }
        else if (Kastle2::hw.JustPressed(Hardware::Button::MODE))
        {
            CancelPreviewMode();
        }
        else
        {
            const int32_t raw_low = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_2);
            const int32_t raw_rhythm = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_3);
            const int32_t raw_contour = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_4);
            const int32_t raw_high = Kastle2::hw.GetRawPotValue(Hardware::Pot::POT_6);
            constexpr int32_t kPreviewPotDeadband = 48;
            const bool low_moved = std::abs(raw_low - preview_pot_baseline_[0]) >= kPreviewPotDeadband;
            const bool rhythm_moved = std::abs(raw_rhythm - preview_pot_baseline_[1]) >= kPreviewPotDeadband;
            const bool contour_moved = std::abs(raw_contour - preview_pot_baseline_[2]) >= kPreviewPotDeadband;
            const bool high_moved = std::abs(raw_high - preview_pot_baseline_[3]) >= kPreviewPotDeadband;
            const int prospective_low = static_cast<int>(std::round(
                PotToScalePosition(raw_low)));
            const int prospective_high = static_cast<int>(std::round(
                PotToScalePosition(raw_high)));
            const int32_t effective_preview_rhythm = tension_patch_.active ?
                tension_cv_value_ : raw_rhythm;
            const int prospective_density = std::min(7, std::max(0,
                static_cast<int>((static_cast<int64_t>(
                    effective_preview_rhythm - POT_MIN) * 8) /
                    (POT_MAX - POT_MIN + 1))));
            const int prospective_contour = MapControlToPosition(raw_contour, 8);
            const bool physical_family_change = !tension_patch_.active &&
                raw_rhythm != preview_pot_baseline_[1];

            const bool low_changed = low_moved && prospective_low != preview_low_;
            const bool high_changed = high_moved && prospective_high != preview_high_;
            if (low_changed || high_changed)
            {
                preview_low_ = prospective_low;
                preview_high_ = prospective_high;
                if (low_moved) preview_pot_baseline_[0] = raw_low;
                if (high_moved) preview_pot_baseline_[3] = raw_high;
                preview_resume_loop_ = preview_loop_;
                reference_frequency_ = ScaleDegreeToFrequency(
                    low_changed ? preview_low_ : preview_high_);
                reference_audition_ = true;
                audition_settle_samples_ = MillisecondsToSamples(980);
                StopAuditionNotes();
                env_.SetHold(true);
                TriggerPreviewSine(low_changed ? preview_low_ : preview_high_);
                const int lo = std::min(preview_low_, preview_high_);
                const int hi = std::max(preview_low_, preview_high_);
                for (int &pitch : preview_pitch_)
                    pitch = std::max(lo, std::min(hi, pitch));
                EnsurePreviewAnchors();
                ApplyPreviewAnchors();
            }
            if (prospective_density != preview_density_)
            {
                if (physical_family_change) TriggerPreviewDetentTok();
                preview_density_ = prospective_density;
                if (!tension_patch_.active) preview_pot_baseline_[1] = raw_rhythm;
                RestartPreviewRhythm();
            }
            else if (rhythm_moved)
            {
                preview_pot_baseline_[1] = raw_rhythm;
                RestartPreviewRhythm(true);
            }
            if (contour_moved && prospective_contour != preview_contour_)
            {
                TriggerPreviewDetentTok();
                preview_contour_ = prospective_contour;
                preview_pot_baseline_[2] = raw_contour;
                GeneratePreviewHome();
                preview_loop_ = PreviewLoop::HOME;
                preview_step_ = 0;
            }
        }
    }
    else if (shift_pressed && Kastle2::hw.JustPressed(Hardware::Button::MODE))
    {
        EnterPreviewMode();
    }
    else if (shift_pressed && !suppress_shift_until_release_ &&
             !shift_audition_ && !shift_home_pending_)
    {
        CancelMyTurn();
        saved_rhythm_ = current_rhythm_;
        saved_pitch_ = current_pitch_;
        saved_timing_ = current_timing_samples_;
        saved_distance_ = current_distance_;
        saved_journey_ = true;
        shift_release_pending_ = false;
        shift_home_pending_ = true;
    }
    if (!shift_pressed)
    {
        shift_home_pending_ = false;
        if (shift_audition_)
        {
            // Finish a LOW/HIGH audition even when SHIFT is released.
            shift_release_pending_ = true;
        }
    }
    if (shift_pressed && !suppress_shift_until_release_)
    {
        const float new_low = PotToScalePosition(low_pot_->GetValue());
        const float new_high = PotToScalePosition(high_pot_->GetValue());
        const int new_rhythm = rhythm_pot_->GetMappedValue();
        if (rhythm_pot_->HasMoved() && new_rhythm != rhythm_)
        {
            rhythm_ = new_rhythm;
        }
        if (low_pot_->HasMoved() && std::abs(new_low - low_position_) > 0.002f)
        {
            CancelMyTurn();
            low_position_ = new_low;
            low_ = static_cast<int>(std::round(new_low));
            shift_audition_ = true;
            shift_home_pending_ = false;
            audition_settle_samples_ = MillisecondsToSamples(980);
            const bool was_reference = reference_audition_;
            reference_frequency_ = ReferenceFrequency(low_position_);
            reference_audition_ = true;
            osc_.SetNativeFrequency(
                freq_to_q31(reference_frequency_, SAMPLE_RATE));
            if (!was_reference)
            {
                StopAuditionNotes();
                pending_degree_ = low_;
                env_.SetHold(true);
                TriggerPendingNote();
            }
            else
            {
                const float frequency = reference_frequency_;
                const auto &definition =
                    kVoicePalette[static_cast<size_t>(active_voice_a_)];
                SetDreamTargets(frequency, definition.layer_ratio,
                    dream_a_current_, dream_a_target_,
                    dream_a_layer_current_, dream_a_layer_target_, dream_a_ready_);
            }
        }
        if (high_pot_->HasMoved() && std::abs(new_high - high_position_) > 0.002f)
        {
            CancelMyTurn();
            high_position_ = new_high;
            high_ = static_cast<int>(std::round(new_high));
            shift_audition_ = true;
            shift_home_pending_ = false;
            audition_settle_samples_ = MillisecondsToSamples(980);
            const bool was_reference = reference_audition_;
            reference_frequency_ = ReferenceFrequency(high_position_);
            reference_audition_ = true;
            osc_.SetNativeFrequency(
                freq_to_q31(reference_frequency_, SAMPLE_RATE));
            if (!was_reference)
            {
                StopAuditionNotes();
                pending_degree_ = high_;
                env_.SetHold(true);
                TriggerPendingNote();
            }
            else
            {
                const float frequency = reference_frequency_;
                const auto &definition =
                    kVoicePalette[static_cast<size_t>(active_voice_a_)];
                SetDreamTargets(frequency, definition.layer_ratio,
                    dream_a_current_, dream_a_target_,
                    dream_a_layer_current_, dream_a_layer_target_, dream_a_ready_);
            }
        }
    }

    UpdateAttensionLeds(shift_pressed);
}


// ------------------------------------------------------------
// MIDI
// ------------------------------------------------------------

void AppTemplate::MidiCallback(
    [[maybe_unused]] midi::Message *msg)
{
}


void AppTemplate::ConfigureVoiceBLab(
    int candidate,
    float frequency)
{
    if (candidate < 1 || candidate > kVoicePositions) return;
    const auto character = static_cast<VoiceCharacter>(candidate - 1);
    const auto &definition = kVoicePalette[static_cast<size_t>(character)];
    ConfigureVoice(character, harmony_osc_, voice_b_layer_osc_,
                   voice_b_fm_, harmony_env_);
    SetDreamTargets(frequency, definition.layer_ratio,
        dream_b_current_, dream_b_target_,
        dream_b_layer_current_, dream_b_layer_target_, dream_b_ready_);
    harmony_osc_.SetNativeFrequency(dream_b_current_);
    voice_b_layer_osc_.SetNativeFrequency(dream_b_layer_current_);
    voice_b_fm_.SetNativeFrequency(dream_b_current_);
    if (character == VoiceCharacter::ODDITY)
    {
        voice_b_oddity_glint_.SetNativeFrequency(
            freq_to_q31(frequency * 3.0f, SAMPLE_RATE));
        if (active_voice_a_ == VoiceCharacter::ODDITY)
        {
            // A has ornament priority on simultaneous Oddity notes.
            voice_b_oddity_glint_delay_ = 0;
            voice_b_oddity_glint_remaining_ = 0;
        }
        else
        {
            voice_b_oddity_glint_delay_ = 968;
            voice_b_oddity_glint_remaining_ = 2288;
            voice_b_oddity_glint_ramp_ = 0;
            voice_b_oddity_glint_envelope_ = 0;
        }
    }
}


FASTCODE q15_t AppTemplate::ProcessVoiceBLab(q15_t envelope)
{
    if (voice_b_lab_off_ || voice_b_lab_active_selection_ <= 0)
    {
        return Q15_ZERO;
    }

    const auto character = static_cast<VoiceCharacter>(
        voice_b_lab_active_selection_ - 1);
    const auto &definition = kVoicePalette[static_cast<size_t>(character)];
    // Keep base/layer phase advancing for every character: those oscillators
    // can become audible on the next B-character selection, and freezing
    // them here would turn selection changes into phase discontinuities.
    const q15_t base = q31_to_q15(harmony_osc_.Process());
    const q15_t layer = q31_to_q15(voice_b_layer_osc_.Process());
    int32_t output;
    if (character == VoiceCharacter::ODDITY)
    {
        output = q15_mult(base, q15(0.72f)) +
            q15_mult(layer, q15(0.28f));
        if (active_voice_a_ != VoiceCharacter::ODDITY &&
            voice_b_oddity_glint_delay_ > 0)
        {
            --voice_b_oddity_glint_delay_;
        }
        else if (active_voice_a_ != VoiceCharacter::ODDITY &&
                 voice_b_oddity_glint_remaining_ > 0)
        {
            constexpr q31_t kGlintRampIncrement = 67108863;
            constexpr q31_t kGlintDecayDecrement = 951899;
            if (voice_b_oddity_glint_ramp_ < 32)
            {
                voice_b_oddity_glint_envelope_ = std::min<q31_t>(
                    Q31_MAX, voice_b_oddity_glint_envelope_ + kGlintRampIncrement);
                ++voice_b_oddity_glint_ramp_;
            }
            else
            {
                voice_b_oddity_glint_envelope_ = std::max<q31_t>(
                    0, voice_b_oddity_glint_envelope_ - kGlintDecayDecrement);
            }
            const q15_t level = q15_mult(q15(0.11f),
                q31_to_q15(voice_b_oddity_glint_envelope_));
            output += q15_mult(q31_to_q15(voice_b_oddity_glint_.Process()), level);
            --voice_b_oddity_glint_remaining_;
        }
    }
    else if (definition.fm_index > 0.0f)
    {
        output = q31_to_q15(voice_b_fm_.Process());
    }
    else
    {
        output = static_cast<int32_t>(base) +
            q15_mult(layer, static_cast<q15_t>(definition.layer_level * Q15_MAX));
    }
    output = std::clamp<int32_t>(output, -32768, 32767);
    const q15_t output_level = character == VoiceCharacter::SYNTH ? 19728 :
        static_cast<q15_t>(definition.output_level * Q15_MAX);
    output = q15_mult(static_cast<q15_t>(output), output_level);
    return q15_mult(static_cast<q15_t>(output), envelope);
}
