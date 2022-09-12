//
// Created by Paul Walker on 8/29/22.
//

#ifndef SURGE_RACK_SURGEVCF_HPP
#define SURGE_RACK_SURGEVCF_HPP

#include "SurgeXT.hpp"
#include "XTModule.hpp"
#include "rack.hpp"
#include <cstring>
#include "DebugHelpers.h"


namespace sst::surgext_rack::vcf
{
struct VCFTypeParamQuanity : rack::ParamQuantity
{
    std::string getLabel() override
    {
        return "Filter Model";
    }
    std::string getDisplayValueString() override {
        int val = (int)std::round(getValue());
        return sst::filters::filter_type_names[val];
    }
};

struct VCFSubTypeParamQuanity : rack::ParamQuantity
{
    std::string getLabel() override
    {
        return "Filter SubType";
    }
    std::string getDisplayValueString() override;
};

struct VCF : public modules::XTModule
{
    static constexpr int n_vcf_params{5};
    static constexpr int n_mod_inputs{4};
    static constexpr int n_arbitrary_switches{4};

    enum ParamIds
    {
        FREQUENCY,
        RESONANCE,
        IN_GAIN,
        MIX,
        OUT_GAIN,

        VCF_MOD_PARAM_0,

        VCF_TYPE = VCF_MOD_PARAM_0 + n_vcf_params * n_mod_inputs,
        VCF_SUBTYPE,
        NUM_PARAMS
    };
    enum InputIds
    {
        INPUT_L,
        INPUT_R,

        VCF_MOD_INPUT,
        NUM_INPUTS = VCF_MOD_INPUT + n_mod_inputs,

    };
    enum OutputIds
    {
        OUTPUT_L,
        OUTPUT_R,
        NUM_OUTPUTS
    };
    enum LightIds
    {
        NUM_LIGHTS
    };

    static int modulatorIndexFor(int baseParam, int modulator)
    {
        int offset = baseParam - FREQUENCY;
        return VCF_MOD_PARAM_0 + offset * n_mod_inputs + modulator;
    }


    std::array<int, sst::filters::num_filter_types> defaultSubtype;

    VCF() : XTModule()
    {
        setupSurgeCommon(NUM_PARAMS, false);
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        // FIXME attach formatters here
        configParam<modules::MidiNoteParamQuantity<69>>(FREQUENCY, -60, 70, 0);
        configParam(RESONANCE, 0, 1, sqrt(2) * 0.5, "Resonance", "%", 0.f, 100.f);
        configParam<modules::DecibelParamQuantity>(IN_GAIN, 0, 2, 1);
        configParam(MIX, 0, 1, 1, "Mix", "%", 0.f, 100.f);
        configParam<modules::DecibelParamQuantity>(OUT_GAIN, 0, 2, 1);

        configParam<VCFTypeParamQuanity>(VCF_TYPE, 0, sst::filters::num_filter_types, sst::filters::fut_obxd_4pole);

        int mfst = 0;
        for (auto fc : sst::filters::fut_subcount)
            mfst = std::max(mfst, fc);

        configParam<VCFSubTypeParamQuanity>(VCF_SUBTYPE, 0, mfst, 3);

        for (int i = 0; i < n_vcf_params * n_mod_inputs; ++i)
        {
            configParam(VCF_MOD_PARAM_0 + i, -1, 1, 0);
        }

        setupSurge();

        for (auto &st : defaultSubtype)
            st = 0;
        defaultSubtype[sst::filters::fut_obxd_4pole] = 3;
        defaultSubtype[sst::filters::fut_lpmoog] = 3;
        defaultSubtype[sst::filters::fut_comb_pos] = 1;
        defaultSubtype[sst::filters::fut_comb_neg] = 1;
    }

    std::string getName() override { return "VCF"; }


    static constexpr int nQFUs = MAX_POLY >> 1; // >> 2 for SIMD <<1 for stereo
    sst::filters::QuadFilterUnitState qfus[nQFUs];
    __m128 qfuRCache[sst::filters::n_filter_registers][nQFUs];
    sst::filters::FilterCoefficientMaker<SurgeStorage> coefMaker[nQFUs];
    float delayBuffer[nQFUs][4][sst::filters::utilities::MAX_FB_COMB +
                                sst::filters::utilities::SincTable::FIRipol_N];

    void setupSurge()
    {
        processPosition = BLOCK_SIZE;

        restackSIMD();

        for (auto c = 0; c < nQFUs; ++c)
        {
            coefMaker[c].setSampleRateAndBlockSize(APP->engine->getSampleRate(), BLOCK_SIZE);
            for (auto i = 0; i < 4; ++i)
            {
                qfus[c].DB[i] = &(delayBuffer[c][i][0]);
            }
        }
    }

    void moduleSpecificSampleRateChange() override
    {
        for (auto c = 0; c < nQFUs; ++c)
        {
            coefMaker[c].setSampleRateAndBlockSize(APP->engine->getSampleRate(), BLOCK_SIZE);
        }
    }

    void resetFilterRegisters()
    {
        for (auto c = 0; c < nQFUs; ++c)
        {
            coefMaker[c].Reset();
            std::fill(qfus[c].R, &qfus[c].R[sst::filters::n_filter_registers], _mm_setzero_ps());
            std::fill(qfus[c].C, &qfus[c].C[sst::filters::n_cm_coeffs], _mm_setzero_ps());
            memcpy(qfuRCache[c], qfus[c].R, sst::filters::n_filter_registers * sizeof(__m128));
            for (int i = 0; i < 4; ++i)
            {
                qfus[c].WP[i] = 0;
                qfus[c].active[i] = 0xFFFFFFFF;
            }
        }
    }

    int processPosition;
    int32_t channelToSIMD[2 /* channels */][MAX_POLY][2 /* register and slot */];
    bool stereoStack{false};
    int nVoices{0}, nSIMDSlots{0};

    int lastPolyL{-2}, lastPolyR{-2};

    void restackSIMD()
    {
        for (int c = 0; c < 2; ++c)
        {
            for (int p = 0; p < MAX_POLY; ++p)
            {
                channelToSIMD[c][p][0] = -1;
                channelToSIMD[c][p][1] = -1;
            }
            stereoStack = false;
            nVoices = 0;
            nSIMDSlots = 0;
        }

        if (lastPolyL == -1 && lastPolyR == -1)
        {
            // blank is fine
        }
        else if (lastPolyR == -1 || lastPolyL == -1)
        {
            int channel = (lastPolyR == -1) ? 0 : 1;
            int poly = (lastPolyR == -1) ? lastPolyL : lastPolyR;

            for (int p = 0; p < poly; ++p)
            {
                channelToSIMD[channel][p][0] = p / 4;
                channelToSIMD[channel][p][1] = p % 4;
            }

            nVoices = poly;
            nSIMDSlots = (poly - 1) / 4 + 1;
        }
        else
        {
            int poly = std::max(lastPolyR, lastPolyL);
            stereoStack = true;
            for (int p = 0; p < poly; p++)
            {
                auto idx = p * 2;
                channelToSIMD[0][p][0] = idx / 4;
                channelToSIMD[0][p][1] = idx % 4;
                channelToSIMD[1][p][0] = (idx + 1) / 4;
                channelToSIMD[1][p][1] = (idx + 1) % 4;
            }
            nVoices = poly * 2;
            nSIMDSlots = (nVoices - 1) / 4 + 1;
        }

        // reset all filters
        resetFilterRegisters();
    }

    sst::filters::FilterType lastType = sst::filters::FilterType::fut_none;
    sst::filters::FilterSubType lastSubType = sst::filters::FilterSubType::st_Standard;
    sst::filters::FilterUnitQFPtr filterPtr{nullptr};

    void process(const typename rack::Module::ProcessArgs &args) override
    {
        static int dumpEvery = 0;
        auto ftype = (sst::filters::FilterType)(int)(std::round(params[VCF_TYPE].getValue()));
        auto fsubtype = (sst::filters::FilterSubType)(int)(std::round(params[VCF_SUBTYPE].getValue()));;

        if (processPosition >= BLOCK_SIZE )
        {
            if (ftype != lastType || fsubtype != lastSubType)
            {
                resetFilterRegisters();
            }
            lastType = ftype;
            lastSubType = fsubtype;
            defaultSubtype[lastType] = lastSubType;
            filterPtr = sst::filters::GetQFPtrFilterUnit(ftype, fsubtype);

            for (int c = 0; c < nQFUs; ++c)
            {
                memcpy(qfuRCache[c], qfus[c].R, sst::filters::n_filter_registers * sizeof(__m128));
            }

            int thisPolyL{-1}, thisPolyR{-1};
            if (inputs[INPUT_L].isConnected())
            {
                thisPolyL = inputs[INPUT_L].getChannels();
            }
            if (inputs[INPUT_R].isConnected())
            {
                thisPolyR = inputs[INPUT_R].getChannels();
            }

            if (thisPolyL != lastPolyL || thisPolyR != lastPolyR)
            {
                lastPolyL = thisPolyL;
                lastPolyR = thisPolyR;

                outputs[OUTPUT_L].setChannels(std::max(1, thisPolyL));
                outputs[OUTPUT_R].setChannels(std::max(1, thisPolyR));

                restackSIMD();
            }

            // Setup Coefficients if changed or modulated etc

            // HAMMER - we can and must be smarter about this
            for (int c = 0; c < nQFUs; ++c)
            {
                for (int f = 0; f < sst::filters::n_cm_coeffs; ++f)
                {
                    coefMaker[c].C[f] = qfus[c].C[f][0];
                }

                coefMaker[c].MakeCoeffs(params[FREQUENCY].getValue(),
                                        params[RESONANCE].getValue(),
                                        ftype, fsubtype, storage.get(), false);
                for (int p = 0; p < 4; ++p)
                {
                    coefMaker[c].updateState(qfus[c], p);
                }
                memcpy(qfus[c].R, qfuRCache[c], sst::filters::n_filter_registers * sizeof(__m128));
            }
            processPosition = 0;
        }
        float invalues alignas(16)[2 * MAX_POLY];
        float outvalues alignas(16)[2 * MAX_POLY];
        // fix me - not every sample pls
        std::memset(invalues, 0, 2 * MAX_POLY * sizeof(float));
        for (int c = 0; c < 2; ++c)
        {
            auto it = (c == 0 ? INPUT_L : INPUT_R);
            auto pl = (c == 0 ? lastPolyL : lastPolyR);
            for (int p = 0; p < pl; ++p)
            {
                auto idx = channelToSIMD[c][p][0] * 4 + channelToSIMD[c][p][1];
                invalues[idx] = inputs[it].getVoltage(p) * RACK_TO_SURGE_OSC_MUL;
            }
        }


        if (filterPtr)
        {
            for (int s = 0; s < nSIMDSlots; ++s)
            {
                auto in = _mm_load_ps(&invalues[s * 4]);

                auto out = filterPtr(&qfus[s], in);
                _mm_store_ps(&outvalues[s * 4], out);
            }
        }
        else
        {
            std::memcpy(outvalues, invalues, nSIMDSlots * 4 * sizeof(float));
        }

        for (int c = 0; c < 2; ++c)
        {
            auto it = (c == 0 ? OUTPUT_L : OUTPUT_R);
            auto pl = (c == 0 ? lastPolyL : lastPolyR);
            for (int p = 0; p < pl; ++p)
            {
                auto idx = channelToSIMD[c][p][0] * 4 + channelToSIMD[c][p][1];
                outputs[it].setVoltage(outvalues[idx] * SURGE_TO_RACK_OSC_MUL, p);
            }
        }

        dumpEvery++;
        if (dumpEvery == 44000)
            dumpEvery = 0;
        processPosition++;
    }


        static std::string subtypeLabel(int type, int subtype)
        {
            using sst::filters::FilterType;
            int i = subtype;
            const auto fType = (FilterType)type;
            if (sst::filters::fut_subcount[type] == 0)
            {
                return "None";
            }
            else
            {
                switch (fType)
                {
                case FilterType::fut_lpmoog:
                case FilterType::fut_diode:
                    return sst::filters::fut_ldr_subtypes[i];
                    break;
                case FilterType::fut_notch12:
                case FilterType::fut_notch24:
                case FilterType::fut_apf:
                    return sst::filters::fut_notch_subtypes[i];
                    break;
                case FilterType::fut_comb_pos:
                case FilterType::fut_comb_neg:
                    return sst::filters::fut_comb_subtypes[i];
                    break;
                case FilterType::fut_vintageladder:
                    return sst::filters::fut_vintageladder_subtypes[i];
                    break;
                case FilterType::fut_obxd_2pole_lp:
                case FilterType::fut_obxd_2pole_hp:
                case FilterType::fut_obxd_2pole_n:
                case FilterType::fut_obxd_2pole_bp:
                    return sst::filters::fut_obxd_2p_subtypes[i];
                    break;
                case FilterType::fut_obxd_4pole:
                    return sst::filters::fut_obxd_4p_subtypes[i];
                    break;
                case FilterType::fut_k35_lp:
                case FilterType::fut_k35_hp:
                    return sst::filters::fut_k35_subtypes[i];
                    break;
                case FilterType::fut_cutoffwarp_lp:
                case FilterType::fut_cutoffwarp_hp:
                case FilterType::fut_cutoffwarp_n:
                case FilterType::fut_cutoffwarp_bp:
                case FilterType::fut_cutoffwarp_ap:
                case FilterType::fut_resonancewarp_lp:
                case FilterType::fut_resonancewarp_hp:
                case FilterType::fut_resonancewarp_n:
                case FilterType::fut_resonancewarp_bp:
                case FilterType::fut_resonancewarp_ap:
                    // "i & 3" selects the lower two bits that represent the stage count
                    // "(i >> 2) & 3" selects the next two bits that represent the
                    // saturator
                    return fmt::format("{} {}", sst::filters::fut_nlf_subtypes[i & 3],
                                       sst::filters::fut_nlf_saturators[(i >> 2) & 3]);
                    break;
                // don't default any more so compiler catches new ones we add
                case FilterType::fut_none:
                case FilterType::fut_lp12:
                case FilterType::fut_lp24:
                case FilterType::fut_bp12:
                case FilterType::fut_bp24:
                case FilterType::fut_hp12:
                case FilterType::fut_hp24:
                case FilterType::fut_SNH:
                    return sst::filters::fut_def_subtypes[i];
                    break;
                case FilterType::fut_tripole:
                    // "i & 3" selects the lower two bits that represent the filter mode
                    // "(i >> 2) & 3" selects the next two bits that represent the
                    // output stage
                    return fmt::format("{} {}", sst::filters::fut_tripole_subtypes[i & 3],
                                       sst::filters::fut_tripole_output_stage[(i >> 2) & 3]);
                    break;
                case FilterType::num_filter_types:
                    return "ERROR";
                    break;
                }
            }
        }
};

inline std::string VCFSubTypeParamQuanity::getDisplayValueString() {
    if (!module)
        return "None";

    int type = (int)std::round(module->params[VCF::VCF_TYPE].getValue());
    int val = (int)std::round(getValue());
    return VCF::subtypeLabel(type, val);
}
}

#endif // SURGE_RACK_SURGEVCF_HPP