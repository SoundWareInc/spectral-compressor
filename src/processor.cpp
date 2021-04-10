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

#include "processor.h"

#include "editor.h"

#include <span>

using juce::uint32;

constexpr char compressor_settings_group_name[] = "compressors";
constexpr char sidechain_active_param_name[] = "sidechain_active";
constexpr char compressor_ratio_param_name[] = "compressor_ratio";
constexpr char compressor_attack_ms_param_name[] = "compressor_attack";
constexpr char compressor_release_ms_param_name[] = "compressor_release";
constexpr char auto_makeup_gain_param_name[] = "auto_makeup_gain";

constexpr char spectral_settings_group_name[] = "spectral";
constexpr char fft_order_param_name[] = "fft_size";
constexpr char windowing_overlap_times_param_name[] = "windowing_times";

SpectralCompressorProcessor::SpectralCompressorProcessor()
    : AudioProcessor(
          BusesProperties()
              .withInput("Input", juce::AudioChannelSet::stereo(), true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)
              .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)),
      parameters(
          *this,
          nullptr,
          "parameters",
          {
              std::make_unique<juce::AudioProcessorParameterGroup>(
                  compressor_settings_group_name,
                  "Compressors",
                  " | ",
                  std::make_unique<juce::AudioParameterBool>(
                      sidechain_active_param_name,
                      "Sidechain Active",
                      false),
                  std::make_unique<juce::AudioParameterFloat>(
                      compressor_ratio_param_name,
                      "Ratio",
                      juce::NormalisableRange<float>(1.0, 300.0, 0.1, 0.25),
                      50.0),
                  std::make_unique<juce::AudioParameterFloat>(
                      compressor_attack_ms_param_name,
                      "Attack",
                      juce::NormalisableRange<float>(0.0, 10000.0, 1.0, 0.2),
                      50.0,
                      " ms",
                      juce::AudioProcessorParameter::genericParameter,
                      [&](float value, int /*max_length*/) -> juce::String {
                          return juce::String(value, 0);
                      }),
                  std::make_unique<juce::AudioParameterFloat>(
                      compressor_release_ms_param_name,
                      "Release",
                      juce::NormalisableRange<float>(0.0, 10000.0, 1.0, 0.2),
                      5000.0,
                      " ms",
                      juce::AudioProcessorParameter::genericParameter,
                      [&](float value, int /*max_length*/) -> juce::String {
                          return juce::String(value, 0);
                      }),
                  std::make_unique<juce::AudioParameterBool>(
                      auto_makeup_gain_param_name,
                      "Auto Makeup Gain",
                      true)),
              std::make_unique<juce::AudioProcessorParameterGroup>(
                  spectral_settings_group_name,
                  "Spectral Settings",
                  " | ",
                  std::make_unique<juce::AudioParameterInt>(
                      fft_order_param_name,
                      "Frequency Resolution",
                      9,
                      15,
                      12,
                      "",
                      [](int value, int /*max_length*/) -> juce::String {
                          return juce::String(1 << value);
                      },
                      [](const juce::String& text) -> int {
                          return std::log2(text.getIntValue());
                      }),
                  // TODO: Change this to disallow non-power of 2 values
                  std::make_unique<juce::AudioParameterInt>(
                      windowing_overlap_times_param_name,
                      "Time Resolution",
                      2,
                      64,
                      4,
                      "x"
                      // TODO: We should show this in the GUI
                      // [&](int value, int /*max_length*/) -> juce::String {
                      //     return juce::String((1 << fft_order) / value);
                      // },
                      // [&](const juce::String& text) -> int {
                      //     return (1 << fft_order) / text.getIntValue();
                      // }
                      )),
          }),
      // TODO: Is this how you're supposed to retrieve non-float parameters?
      //       Seems a bit excessive
      sidechain_active(*dynamic_cast<juce::AudioParameterBool*>(
          parameters.getParameter(sidechain_active_param_name))),
      compressor_ratio(
          *parameters.getRawParameterValue(compressor_ratio_param_name)),
      compressor_attack_ms(
          *parameters.getRawParameterValue(compressor_attack_ms_param_name)),
      compressor_release_ms(
          *parameters.getRawParameterValue(compressor_release_ms_param_name)),
      auto_makeup_gain(*dynamic_cast<juce::AudioParameterBool*>(
          parameters.getParameter(auto_makeup_gain_param_name))),
      fft_order(*dynamic_cast<juce::AudioParameterInt*>(
          parameters.getParameter(fft_order_param_name))),
      windowing_overlap_times(*dynamic_cast<juce::AudioParameterInt*>(
          parameters.getParameter(windowing_overlap_times_param_name))),
      compressor_settings_listener(
          [&](const juce::String& /*parameterID*/, float /*newValue*/) {
              compressor_settings_changed = true;
          }),
      process_data_updater([&]() {
          update_and_swap_process_data();

          const size_t new_window_size = 1 << fft_order;
          setLatencySamples(new_window_size);
      }),
      fft_order_listener(
          [&](const juce::String& /*parameterID*/, float /*newValue*/) {
              process_data_updater.triggerAsyncUpdate();
          }) {
    // TODO: Move the latency computation elsewhere
    const size_t new_window_size = 1 << fft_order;
    setLatencySamples(new_window_size);

    // XXX: There doesn't seem to be a fool proof way to just iterate over all
    //      parameters in a group, right?
    for (const auto& compressor_param_name :
         {sidechain_active_param_name, compressor_ratio_param_name,
          compressor_attack_ms_param_name, compressor_release_ms_param_name,
          auto_makeup_gain_param_name, windowing_overlap_times_param_name}) {
        parameters.addParameterListener(compressor_param_name,
                                        &compressor_settings_listener);
    }

    parameters.addParameterListener(fft_order_param_name, &fft_order_listener);
}

SpectralCompressorProcessor::~SpectralCompressorProcessor() {}

const juce::String SpectralCompressorProcessor::getName() const {
    return JucePlugin_Name;
}

bool SpectralCompressorProcessor::acceptsMidi() const {
#if JucePlugin_WantsMidiInput
    return true;
#else
    return false;
#endif
}

bool SpectralCompressorProcessor::producesMidi() const {
#if JucePlugin_ProducesMidiOutput
    return true;
#else
    return false;
#endif
}

bool SpectralCompressorProcessor::isMidiEffect() const {
#if JucePlugin_IsMidiEffect
    return true;
#else
    return false;
#endif
}

double SpectralCompressorProcessor::getTailLengthSeconds() const {
    return 0.0;
}

int SpectralCompressorProcessor::getNumPrograms() {
    return 1;
}

int SpectralCompressorProcessor::getCurrentProgram() {
    return 0;
}

void SpectralCompressorProcessor::setCurrentProgram(int index) {
    juce::ignoreUnused(index);
}

const juce::String SpectralCompressorProcessor::getProgramName(int /*index*/) {
    return "default";
}

void SpectralCompressorProcessor::changeProgramName(
    int /*index*/,
    const juce::String& /*newName*/) {}

void SpectralCompressorProcessor::prepareToPlay(
    double /*sampleRate*/,
    int maximumExpectedSamplesPerBlock) {
    max_samples_per_block = static_cast<uint32>(maximumExpectedSamplesPerBlock);

    // TODO: We may be doing double work here when `process_data_updater`
    //       changes the latency and the host restarts playback
    // After initializing the process data we make an explicit call to
    // `process_data.get()` to swap the two filters in case we get a
    // parameter change before the first processing cycle
    update_and_swap_process_data();
    process_data.get();
}

void SpectralCompressorProcessor::releaseResources() {
    process_data.clear([](ProcessData& process_data) {
        process_data.windowing_function.reset();
        process_data.fft.reset();

        process_data.fft_scratch_buffer.clear();
        process_data.fft_scratch_buffer.shrink_to_fit();
        process_data.spectral_compressors.clear();
        process_data.spectral_compressors.shrink_to_fit();
        process_data.spectral_compressor_sidechain_thresholds.clear();
        process_data.spectral_compressor_sidechain_thresholds.shrink_to_fit();
        process_data.input_ring_buffers.clear();
        process_data.input_ring_buffers.shrink_to_fit();
        process_data.output_ring_buffers.clear();
        process_data.output_ring_buffers.shrink_to_fit();
        process_data.sidechain_ring_buffers.clear();
        process_data.sidechain_ring_buffers.shrink_to_fit();
    });
}

bool SpectralCompressorProcessor::isBusesLayoutSupported(
    const BusesLayout& layouts) const {
    // We can support any number of channels, as long as the main input, main
    // output, and sidechain input have the same number of channels
    const juce::AudioChannelSet sidechain_channel_set =
        layouts.getChannelSet(true, 1);
    return (layouts.getMainInputChannelSet() ==
            layouts.getMainOutputChannelSet()) &&
           (sidechain_channel_set == layouts.getMainInputChannelSet()) &&
           !layouts.getMainInputChannelSet().isDisabled();
}

void SpectralCompressorProcessor::processBlockBypassed(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midiMessages*/) {
    ProcessData& process_data = this->process_data.get();

    // We need to maintain the same latency when bypassed, so we'll reuse most
    // of the processing logic
    do_stft(
        buffer, process_data,
        [this](ProcessData& process_data, size_t input_channels) {
            const size_t windowing_interval =
                process_data.fft_window_size /
                static_cast<size_t>(windowing_overlap_times);

            for (size_t channel = 0; channel < input_channels; channel++) {
                // We don't have a way to directly copy between buffers, but
                // most hosts should not actually hit this bypassed state
                // anyways
                // TODO: At some point, do implement this without using the
                //       scratch buffer
                process_data.input_ring_buffers[channel].copy_last_n_to(
                    process_data.fft_scratch_buffer.data(), windowing_interval);
                process_data.output_ring_buffers[channel].read_n_from_in_place(
                    process_data.fft_scratch_buffer.data(), windowing_interval);
            }
        });
}

void SpectralCompressorProcessor::processBlock(
    juce::AudioBuffer<float>& buffer,
    juce::MidiBuffer& /*midiMessages*/) {
    ProcessData& process_data = this->process_data.get();

    // We'll update the compressor settings just before processing if the
    // settings have changed or if the sidechaining has been disabled
    bool expected = true;
    if (compressor_settings_changed.compare_exchange_strong(expected, false)) {
        update_compressors(process_data);
    }

    // This function will let us process the input signal in windows, using
    // overlap-add
    do_stft(
        buffer, process_data,
        [this](ProcessData& process_data, size_t input_channels) {
            // If sidechaining is active, we set the compressor thresholds based
            // on a sidechain signal. Since compression is already ballistics
            // based we don't need any additional smoothing here.
            if (sidechain_active) {
                for (size_t channel = 0; channel < input_channels; channel++) {
                    process_data.sidechain_ring_buffers[channel].copy_last_n_to(
                        process_data.fft_scratch_buffer.data(),
                        process_data.fft_window_size);
                    // TODO: We can skip negative frequencies here, right?
                    process_data.fft->performRealOnlyForwardTransform(
                        process_data.fft_scratch_buffer.data(), true);

                    // The version below is better annotated
                    std::span<std::complex<float>> fft_buffer(
                        reinterpret_cast<std::complex<float>*>(
                            process_data.fft_scratch_buffer.data()),
                        process_data.fft_window_size);
                    for (size_t compressor_idx = 0;
                         compressor_idx <
                         process_data.spectral_compressors.size();
                         compressor_idx++) {
                        const size_t bin_idx = compressor_idx + 1;
                        const float magnitude = std::abs(fft_buffer[bin_idx]);

                        // We'll set the compressor threshold based on the
                        // arithmetic mean of the magnitudes of all channels. As
                        // a slight premature optimization (sorry) we'll reset
                        // these magnitudes after using them to avoid the
                        // conditional here.
                        process_data.spectral_compressor_sidechain_thresholds
                            [compressor_idx] += magnitude;
                    }
                }

                for (size_t compressor_idx = 0;
                     compressor_idx < process_data.spectral_compressors.size();
                     compressor_idx++) {
                    process_data.spectral_compressors[compressor_idx]
                        .setThreshold(
                            process_data
                                .spectral_compressor_sidechain_thresholds
                                    [compressor_idx] /
                            input_channels);
                    process_data.spectral_compressor_sidechain_thresholds
                        [compressor_idx] = 0;
                }
            }

            for (size_t channel = 0; channel < input_channels; channel++) {
                process_data.input_ring_buffers[channel].copy_last_n_to(
                    process_data.fft_scratch_buffer.data(),
                    process_data.fft_window_size);
                process_data.windowing_function->multiplyWithWindowingTable(
                    process_data.fft_scratch_buffer.data(),
                    process_data.fft_window_size);
                process_data.fft->performRealOnlyForwardTransform(
                    process_data.fft_scratch_buffer.data());

                // We'll compress every FTT bin individually. Bin 0 is the DC
                // offset and should be skipped, and the latter half of the FFT
                // bins should be processed in the same way as the first half
                // but in reverse order. The real and imaginary parts are
                // interleaved, so ever bin spans two values in the scratch
                // buffer. We can 'safely' do this cast so we can use the STL's
                // complex value functions.
                std::span<std::complex<float>> fft_buffer(
                    reinterpret_cast<std::complex<float>*>(
                        process_data.fft_scratch_buffer.data()),
                    process_data.fft_window_size);

                // TODO: It might be nice to add a DC filter, which would be
                //       very cheap since we're already doing FFT anyways
                for (size_t compressor_idx = 0;
                     compressor_idx < process_data.spectral_compressors.size();
                     compressor_idx++) {
                    // We don't have a compressor for the first bin
                    const size_t bin_idx = compressor_idx + 1;

                    // TODO: Are these _really_ exactly the same in the second
                    //       half ergo this single magnitude is sufficient?
                    const float magnitude = std::abs(fft_buffer[bin_idx]);
                    const float compressed_magnitude =
                        process_data.spectral_compressors[compressor_idx]
                            .processSample(channel, magnitude);

                    // We need to scale both the imaginary and real components
                    // of the bins at the start and end of the spectrum by the
                    // same value
                    // TODO: Add stereo linking
                    const float compression_multiplier =
                        magnitude != 0.0f ? compressed_magnitude / magnitude
                                          : 1.0f;

                    // Since we're usign the real-only FFT operations we don't
                    // need to touch the second, mirrored half of the FFT bins
                    fft_buffer[bin_idx] *= compression_multiplier;
                }

                process_data.fft->performRealOnlyInverseTransform(
                    process_data.fft_scratch_buffer.data());
                process_data.windowing_function->multiplyWithWindowingTable(
                    process_data.fft_scratch_buffer.data(),
                    process_data.fft_window_size);

                // After processing the windowed data, we'll add it to our
                // output ring buffer with any (automatic) makeup gain applied
                // TODO: We might need some kind of optional limiting stage to
                //       be safe
                // TODO: We should definitely add a way to recover transients
                //       from the original input audio, that sounds really good
                process_data.output_ring_buffers[channel].add_n_from_in_place(
                    process_data.fft_scratch_buffer.data(),
                    process_data.fft_window_size, makeup_gain);
            }
        });
}

bool SpectralCompressorProcessor::hasEditor() const {
    return true;
}

juce::AudioProcessorEditor* SpectralCompressorProcessor::createEditor() {
    // TODO: Add an editor at some point
    // return new SpectralCompressorEditor(*this);
    return new juce::GenericAudioProcessorEditor(*this);
}

void SpectralCompressorProcessor::getStateInformation(
    juce::MemoryBlock& destData) {
    const std::unique_ptr<juce::XmlElement> xml =
        parameters.copyState().createXml();
    copyXmlToBinary(*xml, destData);
}

void SpectralCompressorProcessor::setStateInformation(const void* data,
                                                      int sizeInBytes) {
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml && xml->hasTagName(parameters.state.getType())) {
        parameters.replaceState(juce::ValueTree::fromXml(*xml));
    }

    // TODO: Should we do this here, is will `prepareToPlay()` always be called
    //       between loading presets and audio processing starting?
    update_and_swap_process_data();

    // TODO: Do parameter listeners get triggered? Or alternatively, can this be
    //       called during playback (without `prepareToPlay()` being called
    //       first)?
    // TODO: Move the latency computation elsewhere
    const size_t new_window_size = 1 << fft_order;
    setLatencySamples(new_window_size);
}

void SpectralCompressorProcessor::update_and_swap_process_data() {
    process_data.modify_and_swap([this](ProcessData& process_data) {
        process_data.fft_window_size = 1 << fft_order;
        process_data.num_windows_processed = 0;
        process_data.windowing_function.emplace(
            process_data.fft_window_size,
            juce::dsp::WindowingFunction<float>::WindowingMethod::hann,
            // TODO: Or should we leave normalization enabled?
            false);
        process_data.fft.emplace(fft_order);

        // JUCE's FFT class interleaves the real and imaginary numbers, so this
        // buffer should be twice the window size in size
        process_data.fft_scratch_buffer.resize(process_data.fft_window_size *
                                               2);

        // Every FFT bin on both channels gets its own compressor, hooray! The
        // `fft_window_size / 2` is because the first bin is the DC offset and
        // shouldn't be compressed, and the bins after the Nyquist frequency are
        // the same as the first half but in reverse order. The compressor
        // settings will be set in `update_compressors()`, which is triggered on
        // the next processing cycle by setting `compressor_settings_changed`
        // below.
        process_data.spectral_compressors.resize(process_data.fft_window_size /
                                                 2);
        process_data.spectral_compressor_sidechain_thresholds.resize(
            process_data.spectral_compressors.size());

        // We use ring buffers to store the samples we'll process using FFT and
        // also to store the samples that should be played back to
        for (auto* ring_buffers : {&process_data.input_ring_buffers,
                                   &process_data.output_ring_buffers,
                                   &process_data.sidechain_ring_buffers}) {
            ring_buffers->resize(
                static_cast<size_t>(getMainBusNumInputChannels()));
            for (auto& ring_buffer : *ring_buffers) {
                ring_buffer.resize(process_data.fft_window_size);
            }
        }

        // After resizing the compressors are uninitialized and should be
        // reinitialized
        compressor_settings_changed = true;
    });
}

void SpectralCompressorProcessor::update_compressors(
    ProcessData& process_data) {
    // TODO: We should probably update the compressors inline in
    //       `processBlock()` (and do the CaS there). These separate loops cause
    //       some bad cache utilization on larger FFT window sizes, and we can
    //       just calculate t he makeup gain at the start of `processBlock()`
    //       since it isn't very expensive.

    const double effective_sample_rate =
        getSampleRate() / (static_cast<double>(process_data.fft_window_size) /
                           windowing_overlap_times);
    for (size_t compressor_idx = 0;
         compressor_idx < process_data.spectral_compressors.size();
         compressor_idx++) {
        auto& compressor = process_data.spectral_compressors[compressor_idx];

        compressor.setRatio(compressor_ratio);
        compressor.setAttack(compressor_attack_ms);
        compressor.setRelease(compressor_release_ms);
        // TODO: This prepare resets the envelope follower, which is not what we
        //       want. In our own compressor we should have a way to just change
        //       the sample rate.
        // TODO: Now that the timings are compensated for changing window
        //       intervals, we might not need this to be configurable anymore
        //       can just leave this fixed at 4x.
        compressor.prepare(juce::dsp::ProcessSpec{
            // We only process everything once every `windowing_interval`,
            // otherwise our attack and release times will be all messed up
            .sampleRate = effective_sample_rate,
            .maximumBlockSize = max_samples_per_block,
            .numChannels = static_cast<uint32>(getMainBusNumInputChannels())});
    }

    // TODO: The user should be able to configure their own slope (or free
    //       drawn)
    // TODO: And we should be doing both upwards and downwards compression,
    //       OTT-style
    constexpr float base_threshold_dbfs = 0.0f;
    if (!sidechain_active) {
        // The thresholds are set to match pink noise.
        // TODO: Change the calculations so that the base threshold parameter is
        //       centered around some frequency
        const float frequency_increment =
            getSampleRate() / process_data.fft_window_size;
        for (size_t compressor_idx = 0;
             compressor_idx < process_data.spectral_compressors.size();
             compressor_idx++) {
            // The first bin doesn't get a compressor
            const size_t bin_idx = compressor_idx + 1;
            const float frequency = frequency_increment * bin_idx;

            // This starts at 1 for 0 Hz (DC)
            const float octave = std::log2(frequency + 2);

            // The 3 dB is to compensate for bin 0
            const float threshold =
                (base_threshold_dbfs + 3.0f) - (3.0f * octave);
            process_data.spectral_compressors[compressor_idx].setThreshold(
                threshold);
        }
    }

    // We need to compensate for the extra gain added by windowing overlap
    // TODO: We should probably also compensate for different FFT window sizes
    makeup_gain = 1.0f / windowing_overlap_times;
    if (auto_makeup_gain) {
        if (sidechain_active) {
            // Not really sure what makes sense here
            // TODO: Take base threshold into account
            makeup_gain *= (compressor_ratio + 24.0f) / 25.0f;
        } else {
            // TODO: Make this smarter, make it take all of the compressor
            //       parameters into account. It will probably start making
            //       sense once we add parameters for the threshold and ratio.
            // FIXME: This makes zero sense! But it works for our current
            //        parameters.
            makeup_gain *=
                (std::log10(compressor_ratio * 100.00f) * 200.0f) - 399.0f;
        }
    }
}

template <typename F>
void SpectralCompressorProcessor::do_stft(juce::AudioBuffer<float>& buffer,
                                          ProcessData& process_data,
                                          F process_fn) {
    juce::ScopedNoDenormals noDenormals;

    juce::AudioBuffer<float> main_io = getBusBuffer(buffer, true, 0);
    juce::AudioBuffer<float> sidechain_io = getBusBuffer(buffer, true, 1);

    const size_t input_channels =
        static_cast<size_t>(getMainBusNumInputChannels());
    const size_t output_channels =
        static_cast<size_t>(getMainBusNumOutputChannels());
    const size_t num_samples = static_cast<size_t>(buffer.getNumSamples());

    // Zero out all unused channels
    for (auto channel = input_channels; channel < output_channels; channel++) {
        buffer.clear(channel, 0.0f, num_samples);
    }

    // We'll process audio in lockstep to make it easier to use processors
    // that require lookahead and thus induce latency. Every this many
    // samples we'll process a new window of input samples. The results will
    // be added to the output ring buffers.
    const size_t windowing_interval =
        process_data.fft_window_size /
        static_cast<size_t>(windowing_overlap_times);

    // We process incoming audio in windows of `windowing_interval`, and
    // when using non-power of 2 buffer sizes of buffers that are smaller
    // than `windowing_interval` it can happen that we have to copy over
    // already processed audio before processing a new window
    const size_t already_processed_samples = std::min(
        num_samples,
        (windowing_interval -
         (process_data.input_ring_buffers[0].pos() % windowing_interval)) %
            windowing_interval);
    const size_t samples_to_be_processed =
        num_samples - already_processed_samples;
    const int windows_to_process = std::ceil(
        static_cast<float>(samples_to_be_processed) / windowing_interval);

    // Since we're processing audio in small chunks, we need to keep track
    // of the current sample offset in `buffers` we should use for our
    // actual audio input and output
    size_t sample_buffer_offset = 0;

    // Copying from the input buffer to our input ring buffer, copying from
    // our output ring buffer to the output buffer, and clearing the output
    // buffer to prevent feedback is always done in sync
    if (already_processed_samples > 0) {
        for (size_t channel = 0; channel < input_channels; channel++) {
            process_data.input_ring_buffers[channel].read_n_from(
                main_io.getReadPointer(channel), already_processed_samples);
            if (process_data.num_windows_processed >= windowing_overlap_times) {
                process_data.output_ring_buffers[channel].copy_n_to(
                    main_io.getWritePointer(channel), already_processed_samples,
                    true);
            } else {
                main_io.clear(channel, 0, already_processed_samples);
            }
            if (sidechain_active) {
                process_data.sidechain_ring_buffers[channel].read_n_from(
                    sidechain_io.getReadPointer(channel),
                    already_processed_samples);
            }
        }

        sample_buffer_offset += already_processed_samples;
    }

    // Now if `windows_to_process > 0`, the current ring buffer position
    // will align with a window and we can start doing our FFT magic
    for (int window_idx = 0; window_idx < windows_to_process; window_idx++) {
        // This is where the actual processing happens
        process_fn(process_data, input_channels);

        // We don't copy over anything to the outputs until we processed a
        // full buffer
        process_data.num_windows_processed += 1;

        // Copy the input audio into our ring buffer and copy the processed
        // audio into the output buffer
        const size_t samples_to_process_this_iteration =
            std::min(windowing_interval, num_samples - sample_buffer_offset);
        for (size_t channel = 0; channel < input_channels; channel++) {
            process_data.input_ring_buffers[channel].read_n_from(
                main_io.getReadPointer(channel) + sample_buffer_offset,
                samples_to_process_this_iteration);
            if (process_data.num_windows_processed >= windowing_overlap_times) {
                process_data.output_ring_buffers[channel].copy_n_to(
                    main_io.getWritePointer(channel) + sample_buffer_offset,
                    samples_to_process_this_iteration, true);
            } else {
                main_io.clear(channel, sample_buffer_offset,
                              samples_to_process_this_iteration);
            }
            if (sidechain_active) {
                process_data.sidechain_ring_buffers[channel].read_n_from(
                    sidechain_io.getReadPointer(channel) + sample_buffer_offset,
                    samples_to_process_this_iteration);
            }
        }

        sample_buffer_offset += samples_to_process_this_iteration;
    }

    jassert(sample_buffer_offset == num_samples);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new SpectralCompressorProcessor();
}
