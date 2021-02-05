/*
 ==============================================================================
 
 This file is part of the JUCE tutorials.
 Copyright (c) 2020 - Raw Material Software Limited
 
 The code included in this file is provided under the terms of the ISC license
 http://www.isc.org/downloads/software-support-policy/isc-license. Permission
 To use, copy, modify, and/or distribute this software for any purpose with or
 without fee is hereby granted provided that the above copyright notice and
 this permission notice appear in all copies.
 
 THE SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES,
 WHETHER EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR
 PURPOSE, ARE DISCLAIMED.
 
 ==============================================================================
 */

/*******************************************************************************
 The block below describes the properties of this PIP. A PIP is a short snippet
 of code that can be read by the Projucer and used to generate a JUCE project.
 
 BEGIN_JUCE_PIP_METADATA
 
 name:             SpectrumAnalyserTutorial
 version:          2.0.0
 vendor:           JUCE
 website:          http://juce.com
 description:      Displays an FFT spectrum analyser.
 
 dependencies:     juce_audio_basics, juce_audio_devices, juce_audio_formats,
 juce_audio_processors, juce_audio_utils, juce_core,
 juce_data_structures, juce_dsp, juce_events, juce_graphics,
 juce_gui_basics, juce_gui_extra
 exporters:        xcode_mac, vs2019, linux_make
 
 type:             Component
 mainClass:        AnalyserComponent
 
 useLocalCopy:     1
 
 END_JUCE_PIP_METADATA
 
 *******************************************************************************/


#pragma once

#include <JuceHeader.h>
#include <chrono>

typedef std::chrono::high_resolution_clock Clock;

class Bar {
public:
    int posX;
    int dataIdx;
    int endIdx;
    float factor;
};

//==============================================================================
class MainComponent   : public juce::AudioAppComponent,
private juce::Timer
{
public:
    MainComponent()
    : forwardFFT (fftOrder),
    window (fftSize, juce::dsp::WindowingFunction<float>::hann)
    {
        setOpaque (true);
        setAudioChannels (2, 0);  // we want a couple of input channels but no outputs
        startTimerHz (60);
        setSize (700, 500);

        // How many notes to group
        // TODO: make that configuragle
        buildTemperedScale(2);
    }
    
    ~MainComponent() override
    {
        shutdownAudio();
    }
    
    //==============================================================================
    void prepareToPlay (int, double sampleRate) override {
        _sampleRate = sampleRate;
    }
    
    void releaseResources() override          {}
    
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override
    {
        if (bufferToFill.buffer->getNumChannels() > 0)
        {
            auto* channelData = bufferToFill.buffer->getReadPointer (0, bufferToFill.startSample);
            
            for (auto i = 0; i < bufferToFill.numSamples; ++i) {
                pushNextSampleIntoFifo (channelData[i]);
            }
        }
    }
    
    //==============================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        g.setOpacity (1.0f);
        g.setColour (juce::Colours::white);
        drawFrame (g);
    }
    
    void timerCallback() override
    {
        if (nextFFTBlockReady)
        {
            drawNextFrameOfSpectrum();
            nextFFTBlockReady = false;
            repaint();
        }
    }
    
    void pushNextSampleIntoFifo (float sample) noexcept
    {
        // if the fifo contains enough data, set a flag to say
        // that the next frame should now be rendered..
        if (fifoIndex == fftSize)               // [11]
        {
            if (!nextFFTBlockReady)            // [12]
            {
                juce::zeromem (fftData, sizeof (fftData));
                memcpy (fftData, fifo, sizeof (fifo));
                nextFFTBlockReady = true;
            }
            
            fifoIndex = 0;
        }
        
        fifo[fifoIndex++] = sample;             // [12]
    }
    
    /**
         * Precalculate the actual X-coordinate on screen for each analyzer bar
         *
         * Since the frequency scale is logarithmic, each position in the X-axis actually represents a power of 10.
         * To improve performace, the position of each frequency is calculated in advance and stored in an array.
         * Canvas space usage is optimized to accommodate exactly the frequency range the user needs.
         * Positions need to be recalculated whenever the frequency range, FFT size or canvas size change.
         *
         *                                 +------------------------ canvas --------------------------+
         *                                   |                                                                         |
         *    |-------------------|-----|-------------|-------------------!-------------------|-------|------------|
         *    1                  10       |            100                  1K                 10K         |           100K (Hz)
         * (10^0)           (10^1)   |          (10^2)               (10^3)              (10^4)   |          (10^5)
         *                              |-------------|<--- logWidth ---->|--------------------------|
         *                  minFreq--> 20                   (pixels)                                22K <--maxFreq
         *                          (10^1.3)                                                     (10^4.34)
         *                           minLog
         */
    void buildTemperedScale(int groupNotes) {
        std::cout << "build tempered scale" << std::endl;
        float root24 = pow(2.0f, 1.0f / 24.0f);
        float c0 = 440.0f * pow(root24, -114.0f); // ~16.35 Hz
        float freq = 0.0f;
        
        int i = 0;
        
        // generate a table of frequencies based on the equal tempered scale
        // https://en.wikipedia.org/wiki/Equal_temperament
        while ((freq = c0 * pow(root24, i)) <= maxFreq) {
            if (freq >= minFreq && i % groupNotes == 0) {
                temperedScale.push_back(freq);
            }
            i++;
        }
        
        int prevBin = 0;
        int prevIdx = 0;
        int nBars = 0;
        
        for (int index = 0; index < temperedScale.size(); index++) {
            float freq = temperedScale[index];
            // which FFT bin best represents this frequency?
            float bin = freqToBin(freq);
            int idx = 0;
            int nextBin = 0;
            
            // start from the last used FFT bin
            if (prevBin > 0 && prevBin + 1 <= bin) {
                idx = prevBin + 1;
            } else {
                idx = bin;
            }
            
            // FFT does not provide many coefficients for low frequencies, so several bars may end up using the same data
            if (idx == prevIdx) {
                nBars++;
            } else {
                // update previous bars using the same index with an interpolation factor
                if (nBars > 1) {
                    for (int iFactor = 0; iFactor < nBars; iFactor++) {
                        float factor = ((float)iFactor + 1) / (float)nBars;
                        allBars[allBars.size() - nBars + iFactor].factor = factor;
                    }
                }
                prevIdx = idx;
                nBars = 1;
            }
            
            prevBin = nextBin = bin;
            
            // check if there's another band after this one
            if (index + 1 < temperedScale.size()) {
                nextBin = freqToBin(temperedScale[index + 1]);
                // and use half the bins in between for this band
                if (nextBin - bin > 1) {
                    prevBin += round((nextBin - bin) / 2);
                }
            }
            
            int endIdx = prevBin - idx > 0 ? prevBin : 0;
            
            Bar bar;
            bar.posX = index;
            bar.dataIdx = idx;
            bar.endIdx = endIdx;
            bar.factor = 0;
            
            allBars.push_back(bar);
        }
    }
    
    float freqToBin(double freq) {
        float bin = round((freq * fftSize) / _sampleRate);
        
        return bin < (fftSize / 2) ? bin : fftSize - 1;
    }
    
    
    void drawNextFrameOfSpectrum()
    {
        // first apply a windowing function to our data
        window.multiplyWithWindowingTable (fftData, fftSize);       // [1]
        
        
        // then render our FFT data..
        forwardFFT.performFrequencyOnlyForwardTransform (fftData);  // [2]
    }
    
    float getLevel(int i, float height) {
        auto mindB = -100.0f;
        auto maxdB =    0.0f;
        
        float newGain = fftData[i];
                
        auto db = juce::jlimit (mindB, maxdB, juce::Decibels::gainToDecibels (newGain) - juce::Decibels::gainToDecibels ((float) fftSize));
        auto level = juce::jmap (db, mindB, maxdB, height, 0.0f);
        
        return level;
    }
    
    void drawFrame (juce::Graphics& g)
    {
        float windowWidth  = getLocalBounds().getWidth();
        float windowHeight = getLocalBounds().getHeight();
        float nBars = allBars.size();
        float barWidth = (windowWidth / temperedScale.size());
        float barSpace = 0.1f;
        float barSpacePx = std::min(barWidth - 1, (barSpace > 0.0f && barSpace < 1.0f) ? barWidth * barSpace : barSpace);
        float width = barWidth - barSpacePx;

        for (int i = 0; i < nBars; i++)
        {
            Bar bar = allBars[i];
            float barHeight = 0.0f;

            if (bar.endIdx == 0) {
                barHeight = getLevel(bar.dataIdx, windowHeight);

                if (bar.factor > 0.0f) {
                    float prevBar = bar.dataIdx > 0 ? getLevel(bar.dataIdx - 1, windowHeight) : barHeight;

                    barHeight = prevBar + (barHeight - prevBar) * bar.factor;
                }
            } else {
                for (int j = bar.dataIdx; j <= bar.endIdx; j++) {
                    if (fftData[j] > barHeight) {
                        barHeight = getLevel(j, windowHeight);
                    }
                }
            }


            float posX = ((float) bar.posX * barWidth) + barSpacePx / 2.0f;
            float adjWidth = width;

            g.fillRect(posX, barHeight, adjWidth, windowHeight - barHeight);
        }
    }
    
    enum
    {
        // NOTE: Try increasing the FFT size and see how FPS' massively drops. Even with an fftOrder of 12.
        // JUCE_USE_VDSP_FRAMEWORK preprocessor flag is set
        // JUCE_COREGRAPHICS_DRAW_ASYNC preprosessor flag is set too
        fftOrder  = 11,
        fftSize   = 1 << fftOrder,  // 2^fftOrder | 11 = 2048 / 12 = 4096 / 13 = 8192 / 14 = 16384 / ...
    };
    
    juce::dsp::FFT forwardFFT;                      // [4]
    juce::dsp::WindowingFunction<float> window;     // [5]
    
    float fifo [fftSize];                           // [6]
    float fftData [2 * fftSize];                    // [7]
    int fifoIndex = 0;                              // [8]
    bool nextFFTBlockReady = false;                 // [9]
    std::vector<float> temperedScale;
    std::vector<Bar> allBars;
    float minLog;
    double _sampleRate;
    float minFreq = 20.0f;
    float maxFreq = 22000.0f;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

