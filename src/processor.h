// Spectral Compressor: an FFT based compressor
// Copyright (C) 2021 Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "ring.h"

/**
 * The number of samples in our FFT window.
 */
constexpr int fft_window_size = 4096;
/**
 * `log2(fft_window_size)`, used to create the FFT processor.
 */
constexpr int fft_order = 12;

static_assert(1 << fft_order == fft_window_size,
              "The FFT order and FFT window sizes don't match up");

/**
 * We'll have to process the input in overlapping windows and add the processed
 * results to a resulting waveform. We'll use four times overlap, so every this
 * many samples we'll do an FFT transformation.
 */
constexpr size_t windowing_interval = fft_window_size / 4;

/**
 * Used to signal to the audio thread that the compressors should be updated
 * first. This contains a reference to the `compressor_settings_changed` field
 * of the processor.
 */
class CompressorSettingsListener
    : public juce::AudioProcessorValueTreeState::Listener {
   public:
    CompressorSettingsListener(std::atomic_bool& compressor_settings_changed);

    void parameterChanged(const juce::String& parameterID,
                          float newValue) override;

   private:
    std::atomic_bool& compressor_settings_changed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CompressorSettingsListener)
};

class SpectralCompressorProcessor : public juce::AudioProcessor {
   public:
    SpectralCompressorProcessor();
    ~SpectralCompressorProcessor() override;

    void prepareToPlay(double sampleRate,
                       int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    void processBlockBypassed(juce::AudioBuffer<float>& buffer,
                              juce::MidiBuffer& midiMessages) override;
    using AudioProcessor::processBlockBypassed;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

   private:
    // We need this stuff for our DSP

    /**
     * Process audio. When the plugin is bypassed we should still compensate for
     * the altency, so if `bypassed` is true we handle audio the exact same way
     * as usual but with all actual processing disabled.
     */
    void process(juce::AudioBuffer<float>& buffer, bool bypassed);

    /**
     * Calculate new compressor thresholds and other settings based on the
     * current parameters.
     */
    void update_compressors();

    /**
     * We'll process the signal with overlapping windows that are added to each
     * other to form the output signal. See `input_ring_buffers` for more
     * information on how we'll do this.
     */
    juce::dsp::WindowingFunction<float> windowing_function;

    /**
     * The FFT processor.
     */
    juce::dsp::FFT fft;

    /**
     * We need a scratch buffer that can contain `fft_window_size * 2` samples.
     */
    std::vector<float> fft_scratch_buffer;

    /**
     * This will contain `(fft_window_size / 2) - 1` compressors. The
     * compressors are already multichannel so we don't need a nested vector
     * here. We'll compress the magnitude of every FFT bin (`sqrt(i^2 + r^2)`)
     * individually, and then scale both the real and imaginary components by
     * the ratio of their magnitude and the compressed value. Bin 0 is the DC
     * offset and the bins in the second half should be processed the same was
     * as the bins in the first half but mirrored.
     */
    std::vector<juce::dsp::Compressor<float>> spectral_compressors;

    /**
     * When setting compressor thresholds based on a sidechain signal we should
     * be taking the average bin magnitudes of all channels. This buffer
     * accumulates `spectral_compressors.size()` threshold values while
     * iterating over the channels of the sidechain signal so we can then
     * average them and configure the compressors based on that.
     */
    std::vector<float> spectral_compressor_sidechain_thresholds;

    /**
     * A ring buffer of size `fft_window_size` for every channel. Every
     * `windowing_interval` we'll copy the last `fft_window_size` samples to
     * `fft_scratch_buffers` using a window function, process it, and then add
     * the results to `output_ring_buffers`.
     */
    std::vector<RingBuffer<float>> input_ring_buffers;
    /**
     * The processed results as described in the docstring of
     * `input_ring_buffers`. Samples from this buffer will be written to the
     * output.
     */
    std::vector<RingBuffer<float>> output_ring_buffers;
    /**
     * These ring buffers are identical to `input_ring_buffers`, but with data
     * from the sidechain input. When sidechaining is enabled, we set the
     * compressor thresholds based on the magnitudes from the same FFT analysis
     * applied to the sidechain input.
     */
    std::vector<RingBuffer<float>> sidechain_ring_buffers;

    // Computed parameters, set indirectly by parameters

    /**
     * Makeup gain to be applied after compression, where 1.0 mean no gain
     * applied. Depends on the current active modes and whether the makeup gain
     * parameters.
     *
     * The computed value also takes the 4x overlap into account.
     */
    float makeup_gain;

    /**
     * Will be set in `CompressorSettingsListener` when any of the compressor
     * related settings change so we can update our compressors. We'll
     * initialize this to true so the compressors will be initialized during the
     * first processing cycle.
     */
    std::atomic_bool compressor_settings_changed = true;

    // Parameters

    juce::AudioProcessorValueTreeState parameters;
    juce::AudioParameterBool& sidechain_active;
    /**
     * Try to automatically compensate for low thresholds. Doesn't do anything
     * when sidechaining is active.
     */
    juce::AudioParameterBool& auto_makeup_gain;

    // Listeners

    CompressorSettingsListener compressor_settings_listener;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpectralCompressorProcessor)
};
