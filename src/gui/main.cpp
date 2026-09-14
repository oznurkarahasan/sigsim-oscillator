// GLFW + OpenGL3 + ImGui + implot real-time scope for the signal-chain
// simulator and its three detectors. This is the only non-headless target
// in the project (Phase 4) -- everything it displays was already verified
// correct headlessly via tools/verify.py before this existed (Phases 2-3).
// Not buildable/runnable without glfw3 + an OpenGL loader + a display; see
// CMakeLists.txt, which skips this target entirely when glfw3/OpenGL
// aren't found so the CLI tools keep building regardless.

#include "ScopeState.h"

#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Copies a ring buffer's payload with a constant Y offset added, preserving
// its circular (size, offset) layout so ImPlot::PlotLine can still walk it
// -- used to stack the bit-decision traces (truth + 3 detectors) on one
// plot without mutating the underlying buffers.
void plotOffsetSeries(const char* label, const RingBuffer& t, const RingBuffer& y,
                       float yOffset, std::vector<float>& scratch) {
    scratch.resize(static_cast<size_t>(y.capacity()));
    const float* src = y.data();
    for (int i = 0; i < y.capacity(); ++i) scratch[static_cast<size_t>(i)] = src[static_cast<size_t>(i)] + yOffset;
    ImPlot::PlotLine(label, t.data(), scratch.data(), y.size(), 0, y.offset());
}

// One SliderFloat/Checkbox/InputInt wired straight to a double/bool/uint32_t
// Params or DetectorParams field, ORing into `changed` so the caller knows
// whether to reconfigure(). Keeps the (long) control panel body readable.
bool sliderD(const char* label, double& field, float lo, float hi, ImGuiSliderFlags flags = 0) {
    float v = static_cast<float>(field);
    if (ImGui::SliderFloat(label, &v, lo, hi, "%.4g", flags)) { field = v; return true; }
    return false;
}
bool sliderI(const char* label, int& field, int lo, int hi) {
    return ImGui::SliderInt(label, &field, lo, hi);
}
bool checkboxB(const char* label, bool& field) { return ImGui::Checkbox(label, &field); }

void drawControlPanel(ScopeState& scope, bool& running, int& scopeWindowMs) {
    ImGui::BeginChild("ControlPanel", ImVec2(380, -1), true);

    if (ImGui::Button(running ? "Pause" : "Run")) running = !running;
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        scope.params() = sigsim::Params{};
        scope.detectorParams() = detect::DetectorParams{};
        scopeWindowMs = 20;
        scope.reconfigure();
        // reconfigure() re-applies the OLD sample count (see comment below on
        // fsChanged) -- resync it to scopeWindowMs against the now-default Fs.
        scope.setWindowSamples(static_cast<int>(scopeWindowMs / 1000.0 * scope.params().sampleRateHz));
    }

    ImGui::Separator();
    ImGui::Text("Live BER (trailing 200 symbols)");
    ImGui::Text("biquad:   %6.2f%%", scope.berBiquad() * 100.0);
    ImGui::Text("goertzel: %6.2f%%", scope.berGoertzel() * 100.0);
    ImGui::Text("autocorr: %6.2f%%", scope.berAutocorr() * 100.0);

    ImGui::Separator();
    if (ImGui::SliderInt("Scope window (ms)", &scopeWindowMs, 5, 200)) {
        const int samples = static_cast<int>(scopeWindowMs / 1000.0 * scope.params().sampleRateHz);
        scope.setWindowSamples(samples);
    }

    bool changed = false;
    sigsim::Params& p = scope.params();
    detect::DetectorParams& dp = scope.detectorParams();

    // Sample rate Fs is the one control-panel field the scope window's
    // sample count depends on but isn't itself: the "Scope window (ms)"
    // slider above only recomputes samples when IT is dragged, so without
    // this, changing Fs silently stretches/shrinks the actual on-screen
    // time span while that slider keeps showing its last, now-wrong value.
    bool fsChanged = false;
    if (ImGui::CollapsingHeader("ADC / sampling", ImGuiTreeNodeFlags_DefaultOpen)) {
        fsChanged = sliderD("Sample rate Fs (Hz)", p.sampleRateHz, 50000.0f, 500000.0f, ImGuiSliderFlags_Logarithmic);
        changed |= fsChanged;
        changed |= sliderI("Oversample", p.oversample, 1, 64);
        changed |= sliderI("ADC bits", p.adcBits, 4, 16);
        changed |= sliderD("Vref (V)", p.vrefV, 0.5f, 5.0f);
    }
    if (ImGui::CollapsingHeader("Noise", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= sliderD("Noise RMS (mV)", p.noiseRmsMv, 0.0f, 500.0f);
    }
    if (ImGui::CollapsingHeader("Analog front end", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= sliderD("RC cutoff (Hz)", p.rcCutoffHz, 1000.0f, 500000.0f, ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("Carrier", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= sliderD("Carrier freq (Hz)", p.signalFreqHz, 1000.0f, 100000.0f);
        changed |= sliderD("Carrier amplitude (mV)", p.signalAmplitudeMv, 0.0f, 500.0f);
        changed |= sliderD("DC offset (V)", p.offsetV, 0.0f, static_cast<float>(p.vrefV));
    }
    if (ImGui::CollapsingHeader("Fading")) {
        changed |= checkboxB("Fade enabled", p.fadeEnabled);
        changed |= sliderD("Fade update (ms)", p.fadeUpdateMs, 1.0f, 500.0f);
        changed |= sliderD("Fade step std", p.fadeStepStd, 0.0f, 1.0f);
        changed |= sliderD("Fade min mult", p.fadeMinMult, 0.001f, 5.0f, ImGuiSliderFlags_Logarithmic);
        changed |= sliderD("Fade max mult", p.fadeMaxMult, 0.1f, 500.0f, ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("OOK modulation", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= checkboxB("OOK enabled", p.ookEnabled);
        changed |= sliderD("OOK on (ms)", p.ookOnMs, 0.1f, 10.0f);
        changed |= sliderD("OOK off (ms)", p.ookOffMs, 0.1f, 10.0f);
    }
    if (ImGui::CollapsingHeader("Seed")) {
        int seed = static_cast<int>(p.rngSeed);
        if (ImGui::InputInt("RNG seed", &seed)) { p.rngSeed = static_cast<uint32_t>(std::max(0, seed)); changed = true; }
    }
    if (ImGui::CollapsingHeader("Detector tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= sliderD("Target freq (Hz)", dp.targetFreqHz, 1000.0f, 100000.0f);
        changed |= sliderD("Threshold frac", dp.thresholdFrac, 0.05f, 0.95f);
        changed |= sliderD("Peak tau (ms)", dp.peakTauMs, 0.5f, 200.0f, ImGuiSliderFlags_Logarithmic);
        changed |= sliderD("Biquad Q", dp.biquadQ, 0.5f, 50.0f);
        changed |= sliderD("Biquad envelope tau (ms)", dp.biquadEnvTauMs, 0.01f, 5.0f, ImGuiSliderFlags_Logarithmic);
        changed |= sliderD("Goertzel block (ms)", dp.goertzelBlockMs, 0.05f, 5.0f, ImGuiSliderFlags_Logarithmic);
        changed |= sliderD("Autocorr window (ms)", dp.autocorrWindowMs, 0.05f, 5.0f, ImGuiSliderFlags_Logarithmic);
    }

    // 4.6: any slider change reconfigures SignalChain/detectors in place
    // (safely re-callable, 3.2) rather than reconstructing anything.
    if (changed) scope.reconfigure();
    // Fs moved: reconfigure() just re-applied the sample count from BEFORE
    // this frame's change, so redo it now against the new Fs to keep the
    // scope's actual time span matching what "Scope window (ms)" displays.
    if (fsChanged) {
        scope.setWindowSamples(static_cast<int>(scopeWindowMs / 1000.0 * p.sampleRateHz));
    }

    ImGui::EndChild();
}

void drawScopePanel(const ScopeState& scope) {
    ImGui::BeginChild("ScopePanel", ImVec2(0, -1), true);
    if (ImPlot::BeginSubplots("##scope", 6, 1, ImVec2(-1, -1), ImPlotSubplotFlags_LinkAllX)) {
        static std::vector<float> scratch;

        // Every plot needs ImPlotAxisFlags_AutoFit on BOTH axes, explicitly,
        // every frame: without it, axes inside a subplot grid fit ONCE
        // (against whatever the first frame's data happened to be, often
        // near-empty) and then hold that range forever -- the scope's real,
        // continuously-scrolling data silently falls outside the fixed
        // view, rendering nothing despite the buffers being entirely
        // correct. (Found by comparing a plot with this flag against five
        // without it, side by side, with identical data.)
        const ImPlotAxisFlags autoFit = ImPlotAxisFlags_AutoFit;

        if (ImPlot::BeginPlot("ADC input")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            ImPlot::PlotLine("adc_v", scope.bufTime().data(), scope.bufAdc().data(),
                              scope.bufAdc().size(), 0, scope.bufAdc().offset());
            ImPlot::PlotLine("ideal_v", scope.bufTime().data(), scope.bufIdeal().data(),
                              scope.bufIdeal().size(), 0, scope.bufIdeal().offset());
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Biquad bandpass + envelope")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            ImPlot::PlotLine("bandpass", scope.bufTime().data(), scope.bufBiquadBandpass().data(),
                              scope.bufBiquadBandpass().size(), 0, scope.bufBiquadBandpass().offset());
            ImPlot::PlotLine("envelope", scope.bufTime().data(), scope.bufBiquadEnvelope().data(),
                              scope.bufBiquadEnvelope().size(), 0, scope.bufBiquadEnvelope().offset());
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Goertzel magnitude")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            ImPlot::PlotStairs("goertzel", scope.bufTime().data(), scope.bufGoertzelMag().data(),
                                scope.bufGoertzelMag().size(), 0, scope.bufGoertzelMag().offset());
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Autocorrelation magnitude")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            ImPlot::PlotLine("autocorr", scope.bufTime().data(), scope.bufAutocorrMag().data(),
                              scope.bufAutocorrMag().size(), 0, scope.bufAutocorrMag().offset());
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Fade multiplier (ground truth)")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
            ImPlot::PlotLine("fade_mult", scope.bufTime().data(), scope.bufFadeMult().data(),
                              scope.bufFadeMult().size(), 0, scope.bufFadeMult().offset());
            ImPlot::EndPlot();
        }
        if (ImPlot::BeginPlot("Bit decisions (stacked)")) {
            ImPlot::SetupAxes(nullptr, nullptr, autoFit, autoFit);
            // Vertical offset per row so agreement/disagreement is visually
            // obvious rather than four overlapping 0/1 traces.
            plotOffsetSeries("truth", scope.bufTime(), scope.bufBitTruth(), 0.0f, scratch);
            plotOffsetSeries("biquad", scope.bufTime(), scope.bufBiquadBit(), 1.5f, scratch);
            plotOffsetSeries("goertzel", scope.bufTime(), scope.bufGoertzelBit(), 3.0f, scratch);
            plotOffsetSeries("autocorr", scope.bufTime(), scope.bufAutocorrBit(), 4.5f, scratch);
            ImPlot::EndPlot();
        }
        ImPlot::EndSubplots();
    }
    ImGui::EndChild();
}

} // namespace

int main() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "sigsim detector test bench", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    ScopeState scope;
    sigsim::Params defaultParams;
    detect::DetectorParams defaultDetectorParams;
    int scopeWindowMs = 20;
    scope.configure(defaultParams, defaultDetectorParams,
                     static_cast<int>(scopeWindowMs / 1000.0 * defaultParams.sampleRateHz));

    bool running = true;
    double lastTime = glfwGetTime();
    double accumulatorS = 0.0;
    // Fixed sim step at Fs; clamp how much wall-clock backlog can pile up
    // per frame so a stalled/resized frame doesn't spiral into simulating
    // minutes of backlog on the next good frame (4.3). Capped to ~5
    // frames' worth of real time, not 5 samples -- at Fs=200kHz, 5 samples
    // would just be another way of stalling the scope.
    const double maxCatchupS = 5.0 / 60.0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        const double now = glfwGetTime();
        double dt = now - lastTime;
        lastTime = now;
        dt = std::min(dt, maxCatchupS);

        if (running) {
            accumulatorS = std::min(accumulatorS + dt, maxCatchupS);
            const double simDt = 1.0 / scope.params().sampleRateHz;
            while (accumulatorS >= simDt) {
                scope.advance();
                accumulatorS -= simDt;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(fbW), static_cast<float>(fbH)));
        ImGui::Begin("##root", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        drawControlPanel(scope, running, scopeWindowMs);
        ImGui::SameLine();
        drawScopePanel(scope);

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
