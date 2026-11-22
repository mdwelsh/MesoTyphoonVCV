# Superparasites Dev for VCV Rack 2

This local development module builds the same processor and resources used by
the firmware. It exposes all eight modes and the four mono/stereo, 8-/16-bit
quality settings. It resamples Rack audio to 32 kHz and processes 32-frame
blocks. An unpatched right input takes the left input.

## Build and install

Install a C++ toolchain, Make, jq, and zstd. Download and extract the
[Rack 2 SDK](https://vcvrack.com/downloads/) for your platform and Rack version.
From the repository root, with the git submodules initialized, run:

```sh
make -C rack RACK_DIR=/absolute/path/to/Rack-SDK dist
make -C rack RACK_DIR=/absolute/path/to/Rack-SDK install
```

If the SDK is in `build/rack-sdk/Rack-SDK`, omit `RACK_DIR`. The package is
in `rack/dist/`. The SDK install target copies it into Rack's architecture-specific
plugin directory. Restart Rack and search for **Superparasites Dev**. Rebuild,
reinstall, and restart after editing the DSP.

## Audition a change

Patch an audio source into **In L** (and **In R** for stereo). Patch **Out L/R**
to your monitoring chain. Select the mode with the **Mode** knob. The small
jack directly below each slider or effect knob is its CV input. The panel
groups Position, Density, Size, and Texture into four sliders, with Mix, Pan,
Feedback, and Reverb below them, following the
[Typhoon layout](https://afterlateraudio.com/products/typhoon). The four LEDs
by the mode name show the Typhoon's mode pattern. The **Clock** input accepts a trigger
or clock, and **Trig** fires a manual trigger. Beat Repeat needs incoming clock
pulses to establish tempo. Use **Freeze** or the **Hold** gate input to hold the buffer.

In Rack's **Chaotic Pendulum** mode, patch pitch CV to **V/oct**, a note gate to
**Gate**, and either output to your monitoring chain. The **Trig** button also
opens the envelope while held. Pitch at 0 V with the Pitch knob centered is C4.
The four sliders are attack, decay, sustain, and release. **Amount** blends the
keyed sine with the chaotic generator. **Shape** changes the pendulum's drive
and harmonic balance, with extra knob travel near the transition around 0.7.
**Animate** detunes the pendulum from −100 to +100 cents. Noon is zero, with
finer control around the center. **Fold** adds harmonics after the blend.

For a first patch, use a short attack, medium decay, some sustain, and a short
release. Start with Fold at minimum and Animate at noon to hear the Shape sweep.
Then set Shape low and Amount around halfway to hear Animate's beating clearly.
The voice is mono and sends the same signal to both outputs.
The physical firmware still uses slot eight for ADSR Envelope.

This desktop wrapper approximates the Typhoon's control scaling. Rack input
full scale is ±5 V. The wrapper does not simulate the Typhoon's analog signal
path, ADC smoothing, calibration, or bootloader. Use one Superparasites Dev
instance when auditioning. The shared firmware RNG assumes a single processing
thread and can race if Rack runs several instances on different workers.
