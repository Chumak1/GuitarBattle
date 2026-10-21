Music Masters Arena — Clean Electric Guitar sample bank

The v29 sampler can load an SFZ bank with WAV or FLAC samples. To enable the
recorded Fender-style clean electric-guitar sound, download the SMALL SFZ+FLAC
(or SFZ+WAV) bank from the official FreePats page:

https://freepats.zenvoid.org/ElectricGuitar/clean-electric-guitar.html

The bank is published under Creative Commons CC0 1.0.

Installation:
1. Download the small-size FSBS Electric Guitar Clean #1 bank.
2. Extract its contents into this folder: Source/Sounds/GuitarSamples/
3. Keep the .sfz file and the samples/ subfolder together. Do not rename sample files.
4. Rebuild and run the app. Check the Visual Studio Output window for
   "GUITAR SAMPLES: ... (ready)".

The app prefers an SFZ filename containing "small" when multiple SFZ files are
present. The sampler loads audio into memory before the audio device starts; it
does not read files from the real-time audio callback. MIDI backing ON/OFF, the
MIDI VOLUME slider, clean input used by scoring, and the v28 fallback tone remain.

Official bank information and license:
https://github.com/freepats/electric-guitar-FSBS-clean
https://creativecommons.org/publicdomain/zero/1.0/
