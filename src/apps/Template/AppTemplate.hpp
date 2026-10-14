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

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/controls/FancyPot.hpp"
#include "common/core/App.hpp"
#include "common/core/Clock.hpp"
#include "common/core/Hardware.hpp"
#include "common/core/Kastle2.hpp"
#include "common/dsp/control/AdsrEnv.hpp"
#include "common/dsp/math/qmath.hpp"
#include "common/dsp/synthesis/Fm2.hpp"
#include "common/dsp/synthesis/Oscillator.hpp"
#include "common/dsp/synthesis/WhiteNoise.hpp"

namespace kastle2
{

class AppTemplate : public virtual App
{
public:
    enum class Mode
    {
        FIRST,
        SECOND,
        THIRD,
        COUNT
    };

    void Init();
    void DeInit();
    FASTCODE void AudioLoop(q15_t *input, q15_t *output, size_t size);
    void UiLoop();
    void MemoryInitialization() {}
    void MidiCallback(midi::Message *msg);

    uint8_t GetId()
    {
        return 0x4F;
    }

private:
    bool inited_ = false;
    Mode mode_;

    // ------------------------------------------------------------
    // PERFORMANCE CONTROL
    // ------------------------------------------------------------

    std::unique_ptr<FancyPot> distance_pot_;
    std::unique_ptr<FancyPot> tension_pot_;
    std::unique_ptr<FancyPot> atmosphere_depth_pot_;
    std::unique_ptr<FancyPot> voice_a_pot_;
    std::unique_ptr<FancyPot> voice_b_pot_;
    std::unique_ptr<FancyPot> atmosphere_pot_;

    static constexpr int kMaxDistance = 12;
    static constexpr int kPhysicalWanderPositions = 13;
    static constexpr int kTensionPositions = 11;
    static constexpr int kVoicePositions = 10;

    // PATCH-BAY MODULATION -------------------------------------------------
    // These intentionally live in the UI/control domain.  An unplugged
    // patch input is indistinguishable from quiet 0 V, so modulation begins
    // only after observed movement and otherwise leaves the physical pot
    // completely authoritative.
    static constexpr uint32_t kPatchObserveWindowMs = 150;
    static constexpr int32_t kPatchActivityThreshold = ADC_5V / 40; // ~0.125 V
    static constexpr uint8_t kPatchActivationWindows = 2;
    // The fast window intentionally ignores very slow sources.  This longer
    // peak-to-peak observation keeps a deliberate sub-Hz TRI alive without
    // making ordinary disconnected ADC noise look like a patch.
    static constexpr uint32_t kPatchSlowObserveWindowMs = 1000;
    static constexpr int32_t kPatchSlowActivityThreshold = ADC_5V / 64; // ~0.078 V
    static constexpr uint8_t kPatchSlowActivationWindows = 2;
    static constexpr uint32_t kPatchInactivityTimeoutMs = 9000;
    static constexpr uint32_t kPatchRecenterMs = 250;
    static constexpr int32_t kContinuousPatchSmoothingDivisor = 8;
    static constexpr int32_t kPatchManualTakeoverThreshold = ADC_5V / 64;

    struct PatchModState
    {
        bool active = false;
        uint8_t qualifying_windows = 0;
        uint8_t slow_qualifying_windows = 0;
        int32_t window_min = 0;
        int32_t window_max = 0;
        int32_t slow_window_min = 0;
        int32_t slow_window_max = 0;
        int32_t filtered_value = 0;
        int32_t recenter_from = 0;
        uint32_t window_started_ms = 0;
        uint32_t slow_window_started_ms = 0;
        uint32_t last_activity_ms = 0;
        uint32_t recenter_started_ms = 0;
    };

    PatchModState free_patch_;
    PatchModState step_patch_;
    PatchModState presence_patch_;
    PatchModState tension_patch_;
    PatchModState wander_patch_;

    int32_t free_atmos_value_ = 0;
    int32_t step_atmos_value_ = 0;
    int32_t presence_value_ = 0;
    int32_t tension_cv_value_ = 0;
    int32_t wander_cv_value_ = 0;
    int tension_chased_ = 0;
    int wander_chased_ = 0;
    int effective_tension_ = 0;
    int effective_wander_ = 0;
    // The control request is held until a normal phrase boundary adopts it.
    int requested_tension_ = 0;
    int adopted_tension_ = 0;
    int32_t previous_manual_atmos_ = -1;
    int32_t previous_manual_presence_ = -1;
    int32_t previous_manual_tension_raw_ = -1;
    int32_t previous_manual_wander_raw_ = -1;
    int previous_manual_tension_ = -1;
    int previous_manual_wander_ = -1;

    int32_t UpdatePatchMod(PatchModState &state, int32_t cv,
                           int32_t physical_value, uint32_t now_ms,
                           bool smooth);
    static bool DetectManualPatchTakeover(int32_t physical_value,
                                          int32_t &previous_value);
    static void ReleasePatchMod(PatchModState &state, int32_t cv,
                                int32_t physical_value, uint32_t now_ms);
    static int32_t MapPatchCvAroundPot(int32_t cv, int32_t physical_value);
    static int MapControlToPosition(int32_t value, int positions);
    static int ChasePhrasePosition(int current, int target);
    void SamplePatchTargetsForPhrase();
    static uint32_t InterpolateLedColor(const uint32_t *anchors,
                                        size_t anchor_count, int32_t position);
    void UpdateAttensionLeds(bool shift_pressed);
    bool UpdateFoxVisit(uint32_t now_ms, bool direct_user_feedback);

    static constexpr uint8_t kLedTensionMinimumBrightness = 48;
    static constexpr uint8_t kLedTensionMaximumBrightness = 230;
    static constexpr uint8_t kLedPresenceMinimumBrightness = 26;
    static constexpr uint32_t kPreviewCommitLedMs = 180;
    uint32_t preview_led_until_ms_ = 0;
    bool fox_visit_active_ = false;
    bool fox_visit_timer_initialized_ = false;
    uint32_t fox_visit_started_ms_ = 0;
    uint32_t fox_visit_next_ms_ = 0;
    uint32_t fox_visit_rng_ = 0xD1B54A32u;

    int requested_distance_ = 0;
    int current_distance_ = 0;

    enum class TensionPhase : uint8_t
    {
        STABLE = 0,
        BUILD,
        HOLD,
        RESOLVE
    };

    int tension_amount_ = 0;
    TensionPhase tension_phase_ = TensionPhase::STABLE;
    int tension_hold_phrases_ = 0;
    enum class VoiceBResolveSuspension : uint8_t
    {
        NONE,
        AWAITING_FIRST_A_ATTACK,
        AWAITING_SECOND_A_ATTACK
    };
    VoiceBResolveSuspension voice_b_resolve_suspension_ =
        VoiceBResolveSuspension::NONE;
    // Phrase-only coordination for the top-end tension gestures.
    enum class Flourish : uint8_t { NONE = 0, CONSERVATIVE, LEAD_IN, FULL };
    Flourish flourish_ = Flourish::NONE;
    bool flourish_candidate_ = false;
    bool flourish_rub_pending_ = false;
    bool flourish_open_hat_pending_ = false;
    bool tension10_chromatic_lock_ = false;
    q15_t atmosphere_depth_ = Q15_MAX;
    bool ending_requested_ = false;
    bool ending_active_ = false;
    bool ending_cadence_active_ = false;
    bool ending_led_frozen_ = false;
    int32_t ending_led_wander_value_ = 0;
    uint8_t ending_led_tension_ = 0;
    // The cadence keeps already-audible A/B energy for a short direct fade,
    // while its atmosphere send remains silent.
    static constexpr uint32_t kCadenceMelodyReleaseSamples = 26400; // 600 ms at 44 kHz.
    uint32_t cadence_melody_release_samples_ = 0;
    bool ending_finalizing_ = false;
    bool ended_ = false;
    uint8_t ending_plan_ = 0;
    uint8_t previous_ending_plan_ = 0xff;
    // The normal ending presents the stored HOME phrase twice before its
    // dedicated percussion cadence takes ownership.
    uint8_t ending_home_phrases_remaining_ = 0;
    uint8_t ending_cadence_bar_ = 0;
    uint8_t ending_cadence_bars_ = 1;
    uint8_t ending_tension_ = 0;

    // MY TURN is a recurring clean post-tension release. A complete solo
    // draws its next ordinary-phrase interval from this inclusive range.
    static constexpr uint16_t kMyTurnMinArmPhrases = 60;
    static constexpr uint16_t kMyTurnMaxArmPhrases = 90;
    static constexpr q15_t kMyTurnMelodyGain = 0;
    // Asymmetric presentation: the band yields over roughly 186 ms, then
    // returns promptly after the solo.
    static constexpr int32_t kMyTurnFadeOutStep = 4;
    static constexpr int32_t kMyTurnFadeInStep = 16;
    enum class MyTurnPhase : uint8_t {
        NONE, FIRST_PHRASE, SECOND_PHRASE, THIRD_PHRASE, FOURTH_PHRASE
    };
    MyTurnPhase my_turn_phase_ = MyTurnPhase::NONE;
    uint16_t my_turn_normal_phrases_ = 0;
    uint16_t my_turn_arm_phrases_ = kMyTurnMinArmPhrases;
    q15_t my_turn_melody_gain_ = Q15_MAX;

    void UpdateTensionForPhrase();
    void ClearFlourishState();
    void SelectEndingPlan();
    void CancelMyTurn();
    void ArmNextMyTurn();
    void ResetMyTurnPerformance();

    // ------------------------------------------------------------
    // HOME BUILDER CONTROLS
    // ------------------------------------------------------------

    std::unique_ptr<FancyPot> low_pot_;
    std::unique_ptr<FancyPot> rhythm_pot_;
    std::unique_ptr<FancyPot> high_pot_;

    static constexpr int kPitchPositions = 21;
    static constexpr int kHomePitchMinimum = -4;
    static constexpr int kHomePitchMaximum =
        kHomePitchMinimum + kPitchPositions - 1;
    static constexpr int kRhythmPositions = 8;
    int low_ = kHomePitchMinimum;
    int high_ = kHomePitchMaximum;
    int rhythm_ = 0;
    bool startup_home_ready_ = false;
    bool shift_audition_ = false;
    bool shift_home_pending_ = false;
    bool shift_release_pending_ = false;
    // Listener-facing B polish around BREAK IT DOWN. Internal B scheduling and
    // shared Atmosphere input remain independent of this presentation ramp.
    static constexpr int32_t kShiftVoiceBPresentationStep = 5;
    q15_t shift_voice_b_presentation_gain_ = Q15_MAX;
    // A Preview commit happens while SHIFT is still physically down. Do not
    // reinterpret that same press as the normal HOME-breakdown gesture.
    bool suppress_shift_until_release_ = false;
    bool saved_journey_ = false;
    int saved_distance_ = 0;
    std::array<bool, 8> saved_rhythm_{};
    std::array<int, 8> saved_pitch_{};
    std::array<int, 8> saved_timing_{};
    int audition_settle_samples_ = 0;
    bool reference_audition_ = false;
    // Keep continuous pot positions for audition; HOME stores grid degrees.
    float low_position_ = kHomePitchMinimum;
    float high_position_ = kHomePitchMaximum;
    float reference_frequency_ = 0.0f;

    enum class PreviewLoop : uint8_t { NONE, HOME };
    bool preview_mode_ = false;
    PreviewLoop preview_loop_ = PreviewLoop::NONE;
    PreviewLoop preview_resume_loop_ = PreviewLoop::NONE;
    int preview_low_ = kHomePitchMinimum;
    int preview_high_ = kHomePitchMaximum;
    int preview_density_ = 0;
    int preview_contour_ = 0;
    int preview_saved_requested_distance_ = 0;
    size_t preview_step_ = 0;
    std::array<int32_t, 4> preview_pot_baseline_{};
    std::array<bool, 8> preview_rhythm_{};
    std::array<int, 8> preview_pitch_{};
    std::array<int, 8> preview_timing_samples_{};
    uint8_t preview_low_anchor_step_ = 0xFF;
    uint8_t preview_high_anchor_step_ = 0xFF;
    bool preview_pending_note_ = false;
    int preview_pending_delay_samples_ = 0;
    int preview_pending_pitch_ = 0;

    // ------------------------------------------------------------
    // VOICE CHARACTER PALETTE
    // ------------------------------------------------------------

    enum class VoiceCharacter : uint8_t
    {
        SOFT = 0,
        PIANO,
        ORGAN,
        SYNTH,
        PLUCK,
        STRINGS,
        BASS,
        REED,
        BELL,
        ODDITY,
        COUNT
    };

    static_assert(static_cast<int>(VoiceCharacter::COUNT) == kVoicePositions);

    struct VoiceDefinition
    {
        Oscillator::Waveform waveform;
        Oscillator::Waveform layer_waveform;
        float layer_ratio;
        float layer_level;
        float attack;
        float decay;
        float sustain;
        float fm_ratio;
        float fm_index;
        float output_level;
    };

    static constexpr std::array<VoiceDefinition, kVoicePositions>
        kVoicePalette = {{
            // A1 SOFT: gentler shared onset, preserving its keeper body.
            {Oscillator::Waveform::SINE, Oscillator::Waveform::TRI, 2.0f, 0.12f, 0.027f, 1.10f, 0.72f, 0.0f, 0.0f, 1.00f},
            // A2 PIANO: struck triangle/square keyboard voice, no FM.
            {Oscillator::Waveform::TRI, Oscillator::Waveform::SQUARE, 2.0f, 0.14f, 0.002f, 0.78f, 0.0f, 0.0f, 0.0f, 0.88f},
            // A3 ORGAN: open-fifth triangle/sine register, no FM.
            {Oscillator::Waveform::TRI, Oscillator::Waveform::SINE, 1.5f, 0.18f, 0.045f, 1.25f, 0.90f, 0.0f, 0.0f, 0.72f},
            // A4 SYNTH: approved shared timbre; A receives a local output trim.
            {Oscillator::Waveform::SAW, Oscillator::Waveform::SQUARE, 1.005f, 0.18f, 0.006f, 0.52f, 0.62f, 0.0f, 0.0f, 0.669f},
            // A5 PLUCK: keeper timbre with a slightly quicker onset.
            {Oscillator::Waveform::TRI, Oscillator::Waveform::SAW, 2.0f, 0.18f, 0.001f, 0.26f, 0.0f, 0.0f, 0.0f, 0.92f},
            // A6 STRINGS: restrained detuned octave for gentle bowed motion.
            {Oscillator::Waveform::TRI, Oscillator::Waveform::SAW, 2.006f, 0.08f, 0.026f, 1.35f, 0.74f, 0.0f, 0.0f, 0.88f},
            // A7 BASS: sine fundamental with a quiet sub-octave triangle.
            {Oscillator::Waveform::SINE, Oscillator::Waveform::TRI, 0.5f, 0.15f, 0.002f, 0.42f, 0.0f, 0.0f, 0.0f, 0.92f},
            // A8 REED: triangle body and quiet third sine for a hollow woody tone.
            {Oscillator::Waveform::TRI, Oscillator::Waveform::SINE, 3.0f, 0.16f, 0.018f, 1.20f, 0.42f, 0.0f, 0.0f, 0.92f},
            // A9 BELL: clean additive harmonic brightness; no FM path.
            {Oscillator::Waveform::SINE, Oscillator::Waveform::SINE, 3.0f, 0.18f, 0.003f, 1.30f, 0.0f, 0.0f, 0.0f, 0.84f},
            // A10 ODDITY: melodic sine/triangle body with a note-on octave glint.
            {Oscillator::Waveform::SINE, Oscillator::Waveform::TRI, 1.0f, 0.28f, 0.004f, 0.64f, 0.0f, 0.0f, 0.0f, 1.00f}
        }};

    VoiceCharacter selected_voice_a_ = VoiceCharacter::SOFT;
    VoiceCharacter selected_voice_b_ = VoiceCharacter::REED;
    VoiceCharacter active_voice_a_ = VoiceCharacter::SOFT;
    VoiceCharacter active_voice_b_ = VoiceCharacter::REED;
    bool voice_a_lab_off_ = true;
    bool temporary_voice_a_ = false;
    bool temporary_voice_b_ = false;
    bool voice_dials_initialized_ = false;

    // ------------------------------------------------------------
    // SOUND ENGINE — VOICE A
    // ------------------------------------------------------------

    Oscillator osc_;
    Oscillator voice_a_layer_osc_;
    Fm2 voice_a_fm_;
    Oscillator voice_a_oddity_glint_;
    uint16_t voice_a_oddity_glint_delay_ = 0;
    uint16_t voice_a_oddity_glint_remaining_ = 0;
    uint8_t voice_a_oddity_glint_ramp_ = 0;
    q31_t voice_a_oddity_glint_envelope_ = 0;
    q15_t voice_a_layer_level_ = 0;
    q15_t voice_a_output_level_ = 0;
    bool voice_a_uses_fm_ = false;
    AdsrEnv env_;
    q15_t env_value_ = 0;

    // ------------------------------------------------------------
    // SOUND ENGINE — VOICE B
    // ------------------------------------------------------------
    //
    // Independent oscillator and envelope for intelligent harmony.

    Oscillator harmony_osc_;
    Oscillator voice_b_layer_osc_;
    Fm2 voice_b_fm_;
    Oscillator voice_b_oddity_glint_;
    uint16_t voice_b_oddity_glint_delay_ = 0;
    uint16_t voice_b_oddity_glint_remaining_ = 0;
    uint8_t voice_b_oddity_glint_ramp_ = 0;
    q31_t voice_b_oddity_glint_envelope_ = 0;
    AdsrEnv harmony_env_;
    q15_t harmony_env_value_ = 0;
    int voice_b_lab_selection_ = 0;
    int voice_b_lab_active_selection_ = 0;
    bool voice_b_lab_off_ = true;
    // The last final pitch that Voice B actually sounded; -1 means no history.
    int last_voice_b_midi_note_ = -1;
    bool voice_b_phrase_participating_ = false;
    bool voice_b_participation_initialized_ = false;
    uint8_t voice_b_participation_phrases_remaining_ = 0;
    int voice_b_participation_tension_ = -1;
    uint32_t voice_b_participation_rng_ = 0x6D2B79F5u;
    enum class VoiceBArrangement : uint8_t { FOLLOW, HOLD };
    VoiceBArrangement voice_b_arrangement_ = VoiceBArrangement::FOLLOW;
    uint8_t voice_b_hold_phrases_remaining_ = 0;
    uint32_t voice_b_hold_rng_ = 0xA511E9B3u;
    bool voice_b_has_harmony_note_ = false;
    bool voice_b_hold_eligible_ = false;

    // ATMOS uses one 280 ms dry history for the environments that need time.
    static constexpr size_t kAtmosphereTap70 = 3080;
    static constexpr size_t kAtmosphereTap140 = 6160;
    static constexpr size_t kAtmosphereDelaySamples = 12320;
    std::array<int16_t, kAtmosphereDelaySamples> atmosphere_delay_{};
    size_t atmosphere_delay_pos_ = 0, atmosphere_delay_history_ = 0;

    // A compact, persistent stereo field for REVERB. It is separate
    // from the protected dry-history line used by the left Atmosphere effects.
    static constexpr size_t kAtmosphereReverbDelayLeft = 4093;
    static constexpr size_t kAtmosphereReverbDelayRight = 4931;
    std::array<int16_t, kAtmosphereReverbDelayLeft> atmosphere_reverb_left_{};
    std::array<int16_t, kAtmosphereReverbDelayRight> atmosphere_reverb_right_{};
    size_t atmosphere_reverb_left_pos_ = 0;
    size_t atmosphere_reverb_right_pos_ = 0;
    int32_t atmosphere_reverb_damp_left_ = 0;
    int32_t atmosphere_reverb_damp_right_ = 0;
    int32_t atmosphere_reverb_field_left_ = 0;
    int32_t atmosphere_reverb_field_right_ = 0;
    enum class AtmosphereEnvironment : uint8_t
    {
        FRACTURE = 0,
        DISTORT,
        ECHO,
        FLANGE,
        REVERB,
        WHISTLE,
        GLIDE,
        COUNT
    };
    q15_t atmosphere_position_ = 0;
    q15_t atmosphere_position_target_ = 0;
    q15_t atmosphere_effect_strength_ = 0;
    q15_t atmosphere_fracture_weight_ = 0;
    uint32_t atmosphere_lfo_phase_ = 0;
    // WHISTLE is a continuously advancing, note-tracked companion voice.
    Oscillator atmosphere_whistle_osc_;
    q15_t atmosphere_whistle_level_ = Q15_MAX;
    uint8_t atmosphere_whistle_ending_divider_ = 0;
    q15_t atmosphere_fracture_gate_level_ = Q15_MAX;
    q15_t atmosphere_fracture_gate_floor_ = Q15_MAX;
    uint32_t atmosphere_fracture_gate_age_ = 0;
    uint32_t atmosphere_fracture_gate_cut_start_ = 0;
    uint32_t atmosphere_fracture_gate_release_ = 0;
    int32_t atmosphere_fracture_gate_decrement_ = 0;
    bool atmosphere_fracture_gate_active_ = false;
    void StartFractureGate();
    FASTCODE void ProcessAtmosphereReverbCore(int32_t dry);
    FASTCODE void ProcessAtmosphere(int32_t dry, int32_t &wet_l, int32_t &wet_r);
    FASTCODE void ProcessAtmosphereEnvironment(uint8_t environment,
                                               int32_t dry,
                                               int32_t tap70,
                                               int32_t tap140,
                                               int32_t tap280,
                                               int32_t whistle_tone,
                                               int32_t &wet_l,
                                               int32_t &wet_r);

    // DREAM keeps the composition's previous and next legitimate pitches in
    // fixed point. Voice A arrives first; Voice B follows and reconverges.
    q31_t dream_a_current_ = 0, dream_a_target_ = 0;
    q31_t dream_a_layer_current_ = 0, dream_a_layer_target_ = 0;
    q31_t dream_b_current_ = 0, dream_b_target_ = 0;
    q31_t dream_b_layer_current_ = 0, dream_b_layer_target_ = 0;
    bool dream_a_ready_ = false, dream_b_ready_ = false;
    void SetDreamTargets(float frequency, float layer_ratio,
                         q31_t &current, q31_t &target,
                         q31_t &layer_current, q31_t &layer_target,
                         bool &ready);
    FASTCODE void UpdateDreamGlissando();

    // Fourth functional voice: sustained harmonic atmosphere.
    Oscillator atmosphere_osc_;
    AdsrEnv atmosphere_env_;
    q15_t atmosphere_env_value_ = 0;
    int atmosphere_degree_ = 0;
    bool atmosphere_active_ = false;
    q15_t atmosphere_target_ = 0;
    q15_t atmosphere_level_ = 0;
    WhiteNoise atmosphere_noise_;
    Oscillator atmosphere_tone_a_;
    Oscillator atmosphere_tone_b_;
    int atmosphere_recent_degree_ = 0;
    int atmosphere_previous_degree_ = 0;
    uint8_t atmosphere_event_note_ = 0;
    uint32_t atmosphere_event_state_ = 0xA53C91E7u;
    uint32_t atmosphere_wait_samples_ = 0;
    uint32_t atmosphere_event_samples_ = 0;
    uint32_t atmosphere_event_age_ = 0;
    int32_t atmosphere_filter_ = 0;
    uint8_t atmosphere_event_kind_ = 0;

    // ------------------------------------------------------------
    // SOUND ENGINE — PERCUSSION
    // ------------------------------------------------------------
    //
    // One deliberately restrained synthesized thump.  It provides
    // a rhythmic reference at HOME and phrase-ending fills as
    // WANDER grants more compositional permission.

    Oscillator percussion_osc_;
    AdsrEnv percussion_env_;
    q15_t percussion_env_value_ = 0;

    // Preview-selector detent: isolated from music, preview, and drums.
    Oscillator rhythm_detent_body_osc_;
    Oscillator rhythm_detent_glint_osc_;
    uint16_t rhythm_detent_body_remaining_ = 0;
    uint16_t rhythm_detent_glint_delay_ = 0;
    uint16_t rhythm_detent_glint_remaining_ = 0;

    // Simple synthesized hi-hat: short noise burst generated by a
    // lightweight xorshift PRNG, so no extra sample memory is needed.
    // Closed and open articulations share this one envelope and noise stream.
    AdsrEnv hihat_env_;
    q15_t hihat_env_value_ = 0;
    uint32_t hihat_noise_state_ = 0x7A3C59E1u;
    AdsrEnv snare_env_;
    q15_t snare_env_value_ = 0;

    // Living hi-hat phrase state.
    // Pattern is an 8-bit mask: bit N means phrase step N gets a tick.
    uint8_t hihat_pattern_mask_ = 0x55u;
    uint8_t snare_pattern_mask_ = 0;
    int hihat_fill_style_ = 0;
    int hihat_phrase_replans_ = 0;

    // Remembered cymbal restraint: each completed episode arms one inclusive
    // four-to-ten-phrase ordinary interval before the next episode.
    static constexpr uint8_t kCymbalRestraintMinArmPhrases = 4;
    static constexpr uint8_t kCymbalRestraintMaxArmPhrases = 10;
    uint8_t cymbal_restraint_phrases_remaining_ = 0;
    uint8_t cymbal_restraint_normal_phrases_ = 0;
    uint8_t cymbal_restraint_arm_phrases_ = kCymbalRestraintMinArmPhrases;

    // True sub-step fill scheduler.
    // We measure the actual master-clock interval in audio samples and
    // place rapid fill ticks inside that interval.
    int samples_since_clock_ = 0;
    int last_clock_interval_samples_ = 0;
    Clock::Sync clock_timing_sync_type_ = Clock::Sync::COUNT;
    bool clock_timing_running_ = false;
    int hihat_subhit_1_samples_ = -1;
    int hihat_subhit_2_samples_ = -1;
    int hihat_subhit_3_samples_ = -1;
    bool hihat_subhit_1_open_ = false;

    enum class DrumTransition : uint8_t
    {
        NONE = 0,
        OUTWARD,
        INWARD,
        HOME,
        LIVING_DEVELOPMENT
    };
    // A phrase-start look-ahead only; ProcessDistanceJourney still commits
    // the actual musical change at the following phrase boundary.
    DrumTransition drum_transition_ = DrumTransition::NONE;
    bool transition_open_hat_pending_ = false;

    // A fill plan is chosen once at the beginning of each phrase.
    // 0 = no fill; 1..3 = number of phrase-end steps allowed to hit.
    int percussion_fill_length_ = 0;
    uint8_t percussion_pattern_mask_ = 0;
    int percussion_groove_phrases_ = 0;
    bool percussion_groove_changed_ = false;

    // ------------------------------------------------------------
    // MOTIF MEMORY
    // ------------------------------------------------------------

    static constexpr size_t kPhraseLength = 8;

    std::array<bool, kPhraseLength> home_rhythm_;
    std::array<int, kPhraseLength> home_pitch_;
    std::array<int, kPhraseLength> home_timing_samples_;
    int home_family_ = 0;

    std::array<bool, kPhraseLength> current_rhythm_;
    std::array<int, kPhraseLength> current_pitch_;
    std::array<int, kPhraseLength> current_timing_samples_;

    // HOME periodically returns as a structural refrain without changing the
    // actual WANDER journey.
    bool home_refrain_active_ = false;
    uint8_t home_refrain_phrases_remaining_ = 0;
    enum class HomePresentationRegister : uint8_t { NORMAL, LOW, HIGH };
    uint8_t structured_home_length_ = 0;
    uint8_t structured_home_phrase_index_ = 0;
    uint8_t structured_home_pattern_ = 0;
    uint8_t next_home_two_phrase_pattern_ = 0;
    uint8_t next_home_three_phrase_pattern_ = 0;
    uint8_t next_home_four_phrase_pattern_ = 0;
    HomePresentationRegister home_presentation_register_ =
        HomePresentationRegister::NORMAL;
    HomePresentationRegister ordinary_wander_register_ =
        HomePresentationRegister::NORMAL;
    // WANDER register presentation has its own stream so its phrase-level
    // audition cannot perturb the existing composition decisions.
    uint32_t wander_register_rng_ = 0xC2B2AE35u;
    uint8_t wander_register_cooldown_ = 0;
    uint8_t normal_refrains_since_home_ = 0;
    uint8_t home_refrain_defer_ = 0;
    uint32_t voice_a_refrain_rng_ = 0x9E3779B9u;
    uint32_t voice_a_refrain_signature_ = 0;
    uint8_t voice_a_refrain_repeats_ = 0;

    size_t phrase_step_ = 0;

    // ------------------------------------------------------------
    // LIVING DISTANCE MEMORY
    // ------------------------------------------------------------

    std::array<bool, kPhraseLength> local_idea_rhythm_;
    std::array<int, kPhraseLength> local_idea_pitch_;
    std::array<int, kPhraseLength> local_idea_timing_samples_;

    bool living_initialized_ = false;
    bool living_variation_active_ = false;
    uint8_t consecutive_trivial_variations_ = 0;

    int living_hold_phrases_ = 0;

    // ------------------------------------------------------------
    // HOME BUILDER — LIVE REHEARSAL v5 + STRONG HOME TOOLS
    // ------------------------------------------------------------

    // HOME COMPOSER
    // HOME COMPOSER
    // ------------------------------------------------------------

    void GenerateNewHome(bool signature);
    void GenerateHomeRhythm(bool signature);
    void GenerateRhythmFamily(int family, std::array<bool, kPhraseLength> &rhythm,
                               std::array<int, kPhraseLength> &timing,
                               bool signature, bool require_different = false);
    void GenerateHomePitch();
    void StopAuditionNotes();
    void StopMelodyAuditionNotes();
    void BeginCadenceMelodyRelease();
    void RestartEndedPlayback();
    void ReadStartupHomeControls();
    int HomeStepDelay(size_t step) const;
    int RhythmFamilyStepDelay(int family, size_t step) const;
    void EnforceRhythmGrammar();
    float ReferenceFrequency(float position) const;
    static float PotToScalePosition(int32_t value);
    void EnterPreviewMode();
    void CancelPreviewMode();
    void CommitPreviewMode();
    void GeneratePreviewRhythm(bool require_different = false);
    void RestartPreviewRhythm(bool require_different = false);
    void GeneratePreviewHome();
    void EnsurePreviewAnchors();
    void ApplyPreviewAnchors();
    void TriggerPreviewSine(int degree);
    void TriggerPreviewClick();
    void TriggerPreviewDetentTok();

    // DISTANCE ENGINE
    void RestoreHome();

    void ProcessDistanceJourney();

    void StepOutward();
    void StepInward();

    void AddPitchDeparture();
    void AddRhythmDeparture();

    void AddContourDeparture();
    void AddRhythmRestructure();
    void AddRegisterDeparture(bool upward = false);

    bool RestoreOnePitchDifference();
    bool RestoreOneRhythmDifference();

    // ------------------------------------------------------------
    // LIVING DISTANCE
    // ------------------------------------------------------------

    void ResetLivingState();

    void InitializeLivingState();

    void ProcessLivingDistance();

    void PlanOrdinaryWanderRegisterPresentation();

    void GenerateLivingVariation();
    void GenerateLivingVariationCandidate();
    bool IsTrivialLivingVariation() const;

    int GetLivingHomeHold() const;
    int GetLivingVariationHold() const;

    // ------------------------------------------------------------
    // ORGANIC TIMING / NOTE SCHEDULER
    // ------------------------------------------------------------

    bool pending_pickup_ = false;
    int pickup_recovery_samples_ = 0;
    bool pending_note_ = false;
    // A delayed pickup can cross a phrase boundary; retain the scheduling
    // phase so A2 only counts notes planned after RESOLVE began.
    bool pending_note_is_resolve_ = false;
    int pending_note_delay_samples_ = 0;
    int pending_degree_ = 0;
    HomePresentationRegister pending_voice_a_register_ =
        HomePresentationRegister::NORMAL;
    bool pending_harmony_ = false;
    int pending_harmony_degree_ = 0;
    bool pending_harmony_chromatic_ = false;

    void SchedulePhraseNote(size_t step, bool home_plan = false,
                            bool suppress_harmony = false);
    void TriggerPendingNote();
    void BeginStructuredHomePresentation(uint8_t length);
    void AdvanceStructuredHomePresentation();
    void ClearStructuredHomePresentation();
    float VoiceARegisterMultiplier(HomePresentationRegister presentation) const;
    void UpdateVoiceBPhraseParticipation();
    void UpdateVoiceBHoldForPhrase();
    void EnterVoiceBRest();
    bool VoiceBSupportsHold() const;
    int MillisecondsToSamples(int milliseconds) const;

    // ------------------------------------------------------------
    // HARMONY VOICE v1
    // ------------------------------------------------------------

    int ChooseHarmonyDegree(int melody_degree) const;

    void SetVoiceACharacter(VoiceCharacter character);
    void SetVoiceAOutputLevel(VoiceCharacter character);
    void SetVoiceBCharacter(VoiceCharacter character);
    void ConfigureVoice(VoiceCharacter character, Oscillator &oscillator,
                        Oscillator &layer_oscillator, Fm2 &fm,
                        AdsrEnv &envelope);
    void ConfigureVoiceBLab(int candidate, float frequency);
    FASTCODE q15_t ProcessVoiceBLab(q15_t envelope);
    void UpdateVoiceCharactersForPhrase();
    void ResetVoiceARefrainState();
    uint32_t VoiceARefrainSignature(bool home_plan) const;
    void ForceAudibleVoiceAChange();
    void UpdateVoiceARefrainPresentation();

    void UpdateAtmosphereForPhrase();
    int ChooseAtmosphereDegree() const;

    // ------------------------------------------------------------
    // PERCUSSION
    // ------------------------------------------------------------

    void PlanPercussionPhrase();
    void PlanHiHatPhrase();
    void PlanTransitionPercussionPhrase();
    void UpdateCymbalRestraintForPhrase();
    void ArmNextCymbalRestraint();
    bool IsCymbalRestraintActive() const;
    void ScheduleHiHatSubStepFill(size_t step);
    void ProcessHiHatSubStepScheduler();
    void InvalidateClockOwnedDelayedEvents();
    void TriggerPercussionForStep(size_t step);
    void TriggerHiHatForStep(size_t step);
    void TriggerHiHat(bool open);
    void MutePercussionOrnaments();

    // ------------------------------------------------------------
    // HELPERS
    // ------------------------------------------------------------

    PatchModState wander_param3_patch_;

    int ClampPitchDegree(int degree) const;
};

}
