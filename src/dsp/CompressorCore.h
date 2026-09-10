#pragma once

#include <core/MultiCompDSP.hpp>

namespace duskstudio
{
class CompressorCore
{
public:
    CompressorCore() noexcept
    {
        core.setOversampling (0);
        set (Parameter::NoiseEnable, 0.0f);
    }

    void prepare (double sampleRate, int maxBlockSize)
    {
        core.setOversampling (0);
        set (Parameter::NoiseEnable, 0.0f);
        core.prepare (sampleRate, maxBlockSize);
    }

    void reset() { core.reset(); }

    void processBlock (const float* const* input, float* const* output,
                       int numChannels, int numSamples)
    {
        core.processBlock (input, output, numChannels, numSamples);
    }

    void setMode (int value) noexcept       { core.setMode (value); }
    void setBypass (bool value) noexcept    { core.setBypass (value); }
    void setMix (float value) noexcept      { core.setMix (value); }

    void setAutoMakeup (bool value) noexcept
        { set (Parameter::AutoMakeup, value ? 1.0f : 0.0f); }
    void setSidechainHp (float value) noexcept
        { set (Parameter::SidechainHP, value); }

    void setOptoPeakReduction (float value) noexcept
        { set (Parameter::OptoPeakReduction, value); }
    void setOptoGain (float value) noexcept
    {
        const float gainDb = (value - 50.0f) * 0.8f;
        set (Parameter::OptoGain, duskaudio::optoGainDbToKnob (gainDb));
    }
    void setOptoLimit (bool value) noexcept
        { set (Parameter::OptoLimit, value ? 1.0f : 0.0f); }

    void setFetInput (float value) noexcept
        { set (Parameter::FetInput, value); }
    void setFetOutput (float value) noexcept
        { set (Parameter::FetOutput, value); }
    void setFetAttack (float value) noexcept
        { set (Parameter::FetAttack, value); }
    void setFetRelease (float value) noexcept
        { set (Parameter::FetRelease, value); }
    void setFetRatio (int value) noexcept
        { set (Parameter::FetRatio, static_cast<float> (value)); }
    void setFetThreshold (float value) noexcept
        { set (Parameter::FetThreshold, value); }

    void setVcaThreshold (float value) noexcept
        { set (Parameter::VcaThreshold, value); }
    void setVcaRatio (float value) noexcept
        { set (Parameter::VcaRatio, value); }
    void setVcaAttack (float value) noexcept
        { set (Parameter::VcaAttack, value); }
    void setVcaRelease (float value) noexcept
        { set (Parameter::VcaRelease, value); }
    void setVcaOutput (float value) noexcept
        { set (Parameter::VcaOutput, value); }
    void setVcaOverEasy (bool value) noexcept
        { set (Parameter::VcaOverEasy, value ? 1.0f : 0.0f); }
    void setVcaDetectorMode (int value) noexcept
        { set (Parameter::VcaClassicDetector, value != 0 ? 1.0f : 0.0f); }

    void setBusThreshold (float value) noexcept
        { set (Parameter::BusThreshold, value); }
    void setBusRatio (int value) noexcept
        { set (Parameter::BusRatio, static_cast<float> (value)); }
    void setBusAttack (int value) noexcept
        { set (Parameter::BusAttack, static_cast<float> (value)); }
    void setBusRelease (int value) noexcept
        { set (Parameter::BusRelease, static_cast<float> (value)); }
    void setBusMakeup (float value) noexcept
        { set (Parameter::BusMakeup, value); }
    void setBusMix (float value) noexcept
        { set (Parameter::BusMix, value); }

    float getGainReduction() const noexcept { return core.getGainReduction(); }

private:
    using Parameter = duskaudio::MultiCompDSP::Parameter;

    void set (Parameter parameter, float value) noexcept
        { core.setParameter (parameter, value); }

    duskaudio::MultiCompDSP core;
};
} // namespace duskstudio
