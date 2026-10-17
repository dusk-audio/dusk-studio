# SFZ instrument artwork

These are licensed photographs and conventional illustrations, with transparent backgrounds. No AI-generated imagery is included. Cropping, resizing, hand-traced background masks and rotations were performed with ordinary image processing. Repository artwork, when available, takes precedence in the browser.

The adapted images retain the licenses listed below. [credits.json](credits.json) records output hashes and, for the nine images prepared for 0.16, hash-pinned inputs and edit recipes. The replacement sources have pinned Commons page revisions. Existing cutouts used for retouching and collection components are pinned to repository commit `8158dd093f676ef5c569c5d31d81b61f0e386529`.

To reproduce those nine images, install the recorded Pillow version and download each `preparation.input.url` to a separate input directory under its recorded `file` name. Also download collection components that have a `url`. Then run:

```sh
python3 tools/prepare-sfz-artwork.py --input-dir /path/to/inputs --output-dir /path/to/outputs
```

The [preparation tool](../../tools/prepare-sfz-artwork.py) checks every input and output hash. It uses the recorded polygon masks, clone offsets and surface colors; it performs no downloads and does not alter the manifest. The piano input is Wikimedia's PNG rendering of the original SVG. The microphone input is Wikimedia's 500-pixel JPEG preview; its input hash identifies that preview, not the full-size original.

## sfz_piano.png

TheresaKnott. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Piano.svg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File:Piano.svg&oldid=866509947).

Used Wikimedia's raster rendering of the CC0 illustration; cropped to its transparent bounds and resized.

## sfz_organ.png

Dongui. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Hammond_c3_Emilio_Mu%C3%B1oz.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_strings.png

Cornergraf. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Violin-with-bow.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_brass.png

BenP. [CC BY 2.5](https://creativecommons.org/licenses/by/2.5/).

[Original source](https://commons.wikimedia.org/wiki/File:French_horn_front.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_woodwinds.png

Yamaha Corporation. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_Flute_YFL-874W.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_mallets.png

The Metropolitan Museum of Art. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Marimba_MET_MUS805A.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File:Marimba_MET_MUS805A.jpg&oldid=1152590486).

Removed the background with manually traced polygon masks, cropped and resized.

## sfz_acoustic_guitar.png

Jim Hutcheson. [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).

[Original source](https://commons.wikimedia.org/wiki/File:C.F._Martin_GRH_160_or_000-16RGT_cropped.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation. Maker and model marks retouched with the recorded surface colors, cloned regions and masks.

## sfz_electric_guitar.png

Patrick Mueller. [CC BY 2.0](https://creativecommons.org/licenses/by/2.0/).

[Original source](https://commons.wikimedia.org/wiki/File:UNIVOX_Hi-Flier_phase_4_from_c.1977.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation. Maker and model marks retouched with the recorded surface colors, cloned regions and masks.

## sfz_bass.png

Patrick Scales; Djhé. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:70%27s_Fender_Jazz_Bass.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation. Maker and model marks retouched with the recorded surface colors, cloned regions and masks.

## sfz_electric_piano.png

Docrobbie. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Rhodes_MkI_Stage_88_1970.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation. Maker and model marks retouched with the recorded surface colors, cloned regions and masks.

## sfz_synth.png

Leo-setä; Clusternote. [CC BY 2.0](https://creativecommons.org/licenses/by/2.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_DX7.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation. Maker and model marks retouched with the recorded surface colors, cloned regions and masks.

## sfz_voice.png

Zzubnik. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Microphone_gooseneck.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File:Microphone_gooseneck.jpg&oldid=785217700).

Used Wikimedia's 500-pixel preview; removed the background with manually traced polygon masks, cropped and resized.

## sfz_world.png

Sathyadeep. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Sitar3.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_drums.png

Mfaiiazi. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Drumkit-icon.png).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_percussion.png

Nicole Gordine. [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Bongos.jpg).

Cropped, resized, and uniform background removed by conventional image processing; some instruments rotated for framing. No AI generation.

## sfz_collection.png

TheresaKnott; Cornergraf; Mfaiiazi; Dusk Audio. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

See sfz_piano.png, sfz_strings.png and sfz_drums.png credits.

Assembled the credited CC0 piano illustration and public-domain violin and drum cutouts; Dusk Audio dedicates the composition under CC0-1.0.
