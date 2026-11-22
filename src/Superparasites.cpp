#include <rack.hpp>

#include "supercell/dsp/granular_processor.h"
#include "supercell/resources.h"

using namespace rack;

namespace {

constexpr int kDspSampleRate = 32000;
constexpr int kBlockSize = 32;
constexpr float kAudioFullScale = 5.f;
// Preserve Rack's saved slot number. Hardware has a separate ADSR slot.
constexpr int kRackPendulumSlot = 7;

const std::vector<std::string> kModes = {
  "Granular", "Pitch Shift / Time Stretch", "Looping Delay", "Spectral Madness",
  "Oliverb", "Resonestor", "Beat Repeat", "Chaotic Pendulum",
};
static_assert(clouds::PLAYBACK_MODE_ADSR == kRackPendulumSlot, "Update the Rack mode mapping");

const std::array<const char*, 9> kControlNames = {
  "Position", "Size", "Pitch", "Density", "Texture",
  "Mix", "Spread", "Feedback", "Reverb",
};

struct Superparasites : engine::Module {
  enum ParamId { MODE, QUALITY, FREEZE, CAPTURE, CONTROL, NUM_PARAMS = CONTROL + 9 };
  enum InputId { LEFT, RIGHT, CLOCK, FREEZE_GATE, VOCT, CV, NUM_INPUTS = CV + 9 };
  enum OutputId { OUT_LEFT, OUT_RIGHT, NUM_OUTPUTS };

  // The embedded processor starts with zero-initialized state on the STM32.
  alignas(16) std::array<uint8_t, 118784> largeMemory{};
  alignas(16) std::array<uint8_t, 65536 - 128> smallMemory{};
  clouds::GranularProcessor processor{};

  using StereoFrame = dsp::Frame<2>;
  dsp::SampleRateConverter<2> inputConverter, outputConverter;
  dsp::DoubleRingBuffer<StereoFrame, 4096> inputQueue, outputQueue;
  std::array<StereoFrame, kBlockSize> nativeInput{};
  int nativeCount = 0;
  int rackRate = kDspSampleRate;
  dsp::SchmittTrigger clockGate, captureButton, freezeGate;
  bool capturePending = false;

  Superparasites() {
    config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
    configSwitch(MODE, 0, kModes.size() - 1, 0, "Mode", kModes);
    configSwitch(QUALITY, 0, 3, 0, "Quality", {
      "Stereo 16-bit", "Mono 16-bit", "Stereo 8-bit", "Mono 8-bit",
    });
    configSwitch(FREEZE, 0, 1, 0, "Freeze", {"Off", "On"});
    configButton(CAPTURE, "Capture / trigger");

    for (size_t i = 0; i < kControlNames.size(); ++i) {
      configParam(CONTROL + i, 0.f, 1.f, i < 6 ? 0.5f : 0.f, kControlNames[i]);
      configInput(CV + i, std::string(kControlNames[i]) + " CV");
    }
    configInput(LEFT, "Left audio");
    configInput(RIGHT, "Right audio");
    configInput(CLOCK, "Trigger / clock");
    configInput(FREEZE_GATE, "Freeze gate");
    configInput(VOCT, "Pitch V/oct");
    configOutput(OUT_LEFT, "Left audio");
    configOutput(OUT_RIGHT, "Right audio");
    configBypass(LEFT, OUT_LEFT);
    configBypass(RIGHT, OUT_RIGHT);

    processor.Init(largeMemory.data(), largeMemory.size(), smallMemory.data(), smallMemory.size());
    inputConverter.setRates(rackRate, kDspSampleRate);
    outputConverter.setRates(kDspSampleRate, rackRate);
  }

  void onSampleRateChange(const SampleRateChangeEvent& event) override {
    rackRate = std::lround(event.sampleRate);
    inputConverter.setRates(rackRate, kDspSampleRate);
    outputConverter.setRates(kDspSampleRate, rackRate);
    inputQueue.clear();
    outputQueue.clear();
    nativeCount = 0;
    capturePending = false;
  }

  void renderBlock() {
    const int slot = std::lround(params[MODE].getValue());
    const bool pendulum = slot == kRackPendulumSlot;
    processor.set_playback_mode(pendulum
      ? clouds::PLAYBACK_MODE_PENDULUM : static_cast<clouds::PlaybackMode>(slot));
    processor.set_quality(std::lround(params[QUALITY].getValue()));
    processor.Prepare();

    std::array<float, 9> controls;
    for (size_t i = 0; i < controls.size(); ++i) {
      controls[i] = clamp(params[CONTROL + i].getValue() + inputs[CV + i].getVoltage() / 5.f, 0.f, 1.f);
    }

    const float pitchKnob = params[CONTROL + 2].getValue();
    const float pitchCv = inputs[VOCT].getVoltage();
    constexpr int pitchTableEnd = LUT_QUANTIZED_PITCH_SIZE - 1;
    const float pitch = pitchKnob < 1.f
      ? stmlib::Interpolate(clouds::lut_quantized_pitch, pitchKnob, pitchTableEnd)
      : clouds::lut_quantized_pitch[pitchTableEnd];
    auto& p = *processor.mutable_parameters();
    p.position = controls[0];
    p.size = controls[1];
    p.pitch = clamp(pitch
      + 12.f * pitchCv + 2.4f * inputs[CV + 2].getVoltage(), -48.f, 48.f);
    p.density = controls[3];
    p.texture = controls[4];
    p.dry_wet = pendulum ? controls[5] : clamp(1.05f * controls[5] - 0.025f, 0.f, 1.f);
    p.stereo_spread = controls[6];
    p.feedback = controls[7];
    p.reverb = controls[8];
    p.freeze = params[FREEZE].getValue() > 0.5f || freezeGate.isHigh();
    p.capture = capturePending;
    p.gate = clockGate.isHigh() || captureButton.isHigh() || (!pendulum && capturePending);
    capturePending = false;

    p.kammerl.slice_selection = clamp(inputs[CV + 4].getVoltage() / 5.f, 0.f, 1.f);
    p.kammerl.slice_modulation = params[CONTROL + 4].getValue();
    p.kammerl.size_modulation = p.density;
    p.kammerl.probability = p.dry_wet;
    p.kammerl.clock_divider = p.stereo_spread;
    p.kammerl.pitch_mode = p.feedback;
    p.kammerl.distortion = p.reverb;
    p.kammerl.pitch = clamp(pitchKnob + pitchCv / 4.f, 0.f, 1.f);

    clouds::ShortFrame in[kBlockSize], out[kBlockSize]{};
    for (int i = 0; i < kBlockSize; ++i) {
      in[i].l = static_cast<int16_t>(clamp(nativeInput[i].samples[0], -1.f, 1.f) * 32767.f);
      in[i].r = static_cast<int16_t>(clamp(nativeInput[i].samples[1], -1.f, 1.f) * 32767.f);
    }
    processor.Process(in, out, kBlockSize);

    StereoFrame rendered[kBlockSize];
    for (int i = 0; i < kBlockSize; ++i) {
      rendered[i] = {{out[i].l / 32768.f, out[i].r / 32768.f}};
    }
    int consumed = kBlockSize;
    int produced = outputQueue.capacity();
    outputConverter.process(rendered, &consumed, outputQueue.endData(), &produced);
    outputQueue.endIncr(produced);
  }

  void process(const ProcessArgs&) override {
    capturePending |= clockGate.process(inputs[CLOCK].getVoltage(), 0.1f, 1.f);
    capturePending |= captureButton.process(params[CAPTURE].getValue(), 0.1f, 0.5f);
    freezeGate.process(inputs[FREEZE_GATE].getVoltage(), 0.1f, 1.f);

    const float left = inputs[LEFT].getVoltage();
    const float right = inputs[RIGHT].getNormalVoltage(left);
    if (!inputQueue.full()) {
      inputQueue.push({{left / kAudioFullScale, right / kAudioFullScale}});
    }

    const int needed = (kBlockSize - nativeCount) * rackRate / kDspSampleRate;
    if (inputQueue.size() >= static_cast<size_t>(std::max(1, needed))
        && outputQueue.capacity() >= 4096 / 2) {
      int consumed = inputQueue.size();
      int produced = kBlockSize - nativeCount;
      inputConverter.process(inputQueue.startData(), &consumed, nativeInput.data() + nativeCount, &produced);
      inputQueue.startIncr(consumed);
      nativeCount += produced;
      if (nativeCount == kBlockSize) {
        renderBlock();
        nativeCount = 0;
      }
    }

    StereoFrame out{};
    if (!outputQueue.empty()) {
      out = outputQueue.shift();
    }
    outputs[OUT_LEFT].setVoltage(out.samples[0] * kAudioFullScale);
    outputs[OUT_RIGHT].setVoltage(out.samples[1] * kAudioFullScale);
  }
};

constexpr float kPanelWidth = 225.f;
constexpr std::array<float, 4> kEffectColumns = {33.f, 86.f, 139.f, 192.f};
constexpr std::array<int, 4> kSliderControls = {0, 3, 1, 4};
constexpr std::array<const char*, 4> kAdsrLabels = {"Attack", "Decay", "Sustain", "Release"};
constexpr std::array<const char*, 4> kEffectLabels = {"Mix", "Pan", "Feedback", "Reverb"};
constexpr std::array<const char*, 4> kPendulumLabels = {"Amount", "Shape", "Fold", "Animate"};

struct Panel : widget::Widget {
  int mode = 0;

  Panel() {
    box.size = math::Vec(kPanelWidth, RACK_GRID_HEIGHT);
  }

  void draw(const DrawArgs& args) override {
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillPaint(args.vg, nvgLinearGradient(args.vg, 0, 0, 0, box.size.y,
      nvgRGB(34, 34, 36), nvgRGB(20, 20, 21)));
    nvgFill(args.vg);

    nvgBeginPath(args.vg);
    nvgRoundedRect(args.vg, 5.f, 5.f, box.size.x - 10.f, box.size.y - 10.f, 3.f);
    for (float y : {92.f, 226.f, 319.f}) {
      nvgMoveTo(args.vg, 13.f, y);
      nvgLineTo(args.vg, box.size.x - 13.f, y);
    }
    nvgStrokeWidth(args.vg, 0.75f);
    nvgStrokeColor(args.vg, nvgRGB(134, 107, 61));
    nvgStroke(args.vg);

    for (float x : kEffectColumns) {
      for (int tick = 0; tick < 7; ++tick) {
        const float y = 125.f + 10.f * tick;
        const float length = tick % 3 == 0 ? 5.f : 3.f;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, x + 9.f, y);
        nvgLineTo(args.vg, x + 9.f + length, y);
        nvgStrokeColor(args.vg, nvgRGB(157, 126, 72));
        nvgStroke(args.vg);
      }
    }

    // Left-to-right mode indicators follow the Typhoon's four-LED patterns.
    constexpr std::array<unsigned, 8> patterns = {0x1, 0x2, 0x4, 0x8, 0xe, 0xd, 0xb, 0x7};
    for (int i = 0; i < 4; ++i) {
      const bool active = (patterns[mode] & (1u << i)) != 0;
      nvgBeginPath(args.vg);
      nvgCircle(args.vg, 19.f + 8.f * i, 37.f, 2.4f);
      nvgFillColor(args.vg, active ? nvgRGB(255, 192, 91) : nvgRGB(88, 73, 51));
      nvgFill(args.vg);
    }
  }
};

struct AmberSlider : VCVSlider {
  void drawLayer(const DrawArgs& args, int layer) override {
    VCVSlider::drawLayer(args, layer);
    if (layer == 1) {
      const math::Vec center = handle->box.getCenter();
      nvgBeginPath(args.vg);
      nvgRoundedRect(args.vg, center.x - 5.f, center.y - 1.f, 10.f, 2.f, 1.f);
      nvgFillColor(args.vg, nvgRGB(255, 191, 93));
      nvgFill(args.vg);
    }
  }
};

struct SuperparasitesWidget : app::ModuleWidget {
  Panel* faceplate;
  ui::Label* modeName;
  ui::Label* clockLabel;
  std::array<ui::Label*, 4> sliderLabels;
  std::array<ui::Label*, 4> effectLabels;

  SuperparasitesWidget(Superparasites* module) {
    setModule(module);
    faceplate = new Panel;
    setPanel(faceplate);

    label("SUPERPARASITES DEV", 112.5f, 19.f, 180.f, 15.f);
    modeName = label(kModes.front().c_str(), 126.f, 37.f, 155.f, 10.f);

    const float top = 73.f;
    label("Mode", 24.f, 50.f, 46.f);
    addParam(createParamCentered<RoundBlackSnapKnob>(math::Vec(24.f, top), module, Superparasites::MODE));
    label("Quality", 54.f, 50.f, 55.f);
    addParam(createParamCentered<RoundSmallBlackKnob>(math::Vec(54.f, top), module, Superparasites::QUALITY));
    label("Pitch", 84.f, 50.f, 40.f);
    addParam(createParamCentered<RoundBlackKnob>(math::Vec(84.f, top), module, Superparasites::CONTROL + 2));
    input(module, Superparasites::CV + 2, "P CV", 114.f, top, 23.f);
    input(module, Superparasites::VOCT, "V/oct", 144.f, top, 23.f);
    label("Freeze", 175.f, 50.f, 50.f);
    addParam(createParamCentered<CKSS>(math::Vec(175.f, top), module, Superparasites::FREEZE));
    label("Trig", 204.f, 50.f, 36.f);
    addParam(createParamCentered<LEDButton>(math::Vec(204.f, top), module, Superparasites::CAPTURE));

    for (size_t i = 0; i < kEffectColumns.size(); ++i) {
      const int control = kSliderControls[i];
      const float x = kEffectColumns[i];
      sliderLabels[i] = label(kControlNames[control], x, 106.f, 56.f);
      addParam(createParamCentered<AmberSlider>(math::Vec(x, 155.f), module, Superparasites::CONTROL + control));
      addInput(createInputCentered<PJ301MPort>(math::Vec(x, 209.f), module, Superparasites::CV + control));
    }

    for (size_t i = 0; i < kEffectColumns.size(); ++i) {
      const int control = 5 + i;
      const float x = kEffectColumns[i];
      effectLabels[i] = label(kEffectLabels[i], x, 239.f, 59.f);
      addParam(createParamCentered<RoundBlackKnob>(math::Vec(x, 264.f), module, Superparasites::CONTROL + control));
      addInput(createInputCentered<PJ301MPort>(math::Vec(x, 301.f), module, Superparasites::CV + control));
    }

    input(module, Superparasites::LEFT, "In L", 24.f, 354.f, 21.f);
    input(module, Superparasites::RIGHT, "In R", 59.f, 354.f, 21.f);
    clockLabel = input(module, Superparasites::CLOCK, "Clock", 95.f, 354.f, 21.f);
    input(module, Superparasites::FREEZE_GATE, "Hold", 130.f, 354.f, 21.f);
    label("Out L", 166.f, 333.f, 46.f);
    label("Out R", 201.f, 333.f, 46.f);
    addOutput(createOutputCentered<PJ301MPort>(math::Vec(166.f, 354.f), module, Superparasites::OUT_LEFT));
    addOutput(createOutputCentered<PJ301MPort>(math::Vec(201.f, 354.f), module, Superparasites::OUT_RIGHT));
  }

  void step() override {
    if (module) {
      const int mode = std::lround(module->params[Superparasites::MODE].getValue());
      if (faceplate->mode != mode) {
        faceplate->mode = mode;
        modeName->text = kModes[mode];
        const bool pendulum = mode == kRackPendulumSlot;
        clockLabel->text = pendulum ? "Gate" : "Clock";
        for (size_t i = 0; i < sliderLabels.size(); ++i) {
          sliderLabels[i]->text = pendulum ? kAdsrLabels[i] : kControlNames[kSliderControls[i]];
          effectLabels[i]->text = pendulum ? kPendulumLabels[i] : kEffectLabels[i];
        }
      }
    }
    ModuleWidget::step();
  }

  ui::Label* label(const char* text, float x, float y, float width, float size = 10.f) {
    auto* caption = new ui::Label;
    caption->box.pos = math::Vec(x - width / 2.f, y - 10.f);
    caption->box.size = math::Vec(width, 20.f);
    caption->text = text;
    caption->fontSize = size;
    caption->alignment = ui::Label::CENTER_ALIGNMENT;
    caption->color = nvgRGB(238, 204, 142);
    addChild(caption);
    return caption;
  }

  ui::Label* input(Superparasites* module, int id, const char* name, float x, float y, float labelOffset) {
    addInput(createInputCentered<PJ301MPort>(math::Vec(x, y), module, id));
    return label(name, x, y - labelOffset, 46.f);
  }
};

}  // namespace

void init(Plugin* plugin) {
  plugin->addModel(createModel<Superparasites, SuperparasitesWidget>("Superparasites"));
}
