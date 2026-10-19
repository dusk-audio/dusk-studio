# SFZ instrument artwork

These are licensed photographs and conventional illustrations, with transparent backgrounds. No AI-generated imagery is included. Cropping, resizing, hand-traced background masks and rotations were performed with ordinary image processing. Applicable repository artwork takes precedence; individual programs in mixed collections use their own instrument artwork.

The adapted images retain the licenses listed below. [credits.json](credits.json) records output hashes and, for the images with preparation records, hash-pinned inputs and edit recipes. The replacement sources have pinned Commons page revisions. Existing cutouts used for retouching and collection components are pinned to repository commit `8158dd093f676ef5c569c5d31d81b61f0e386529`.

To reproduce the prepared images, install the recorded Pillow version and download each `preparation.input.url` to a separate input directory under its recorded `file` name. Also download collection components that have a `url`. Then run:

```sh
python3 tools/prepare-sfz-artwork.py --input-dir /path/to/inputs --output-dir /path/to/outputs
```

The [preparation tool](../../tools/prepare-sfz-artwork.py) checks every input and output hash. It uses the recorded polygon masks, edge-connected background removal, clone offsets, surface colors, rotations and color adjustments; it performs no downloads and does not alter the manifest. The piano input is Wikimedia's PNG rendering of the original SVG. The microphone input is Wikimedia's 500-pixel JPEG preview; its input hash identifies that preview, not the full-size original.

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

## sfz_upright_bass.png

papapishu. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Double_bass_1.svg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ADouble_bass_1.svg&oldid=976353832).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_harp.png

Museum für Kunst und Gewerbe Hamburg. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Harfe-Museum_f%C3%BCr_Kunst_und_Gewerbe_Hamburg-1995.245.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AHarfe-Museum_f%C3%BCr_Kunst_und_Gewerbe_Hamburg-1995.245.png&oldid=1030054601).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_cello.png

Georg Feitscher. [CC BY 3.0](https://creativecommons.org/licenses/by/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:Cello_front_side.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ACello_front_side.png&oldid=970192307).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_banjo.png

Zwitt95. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:White_Banjo.svg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AWhite_Banjo.svg&oldid=1228899551).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_mandolin.png

Arik Kerman. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Mandolin.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AMandolin.png&oldid=1042041521).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_ukulele.png

Kollektives Schreiben. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Ukulele4.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AUkulele4.png&oldid=1276815950).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_accordion.png

ArtFavor. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Accordion.svg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AAccordion.svg&oldid=989782465).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_harmonica.png

Sarah A Morrigan / missiridia. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Harmonica.svg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AHarmonica.svg&oldid=995710934).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_saxophone.png

File:Yamaha Saxophone YAS-62.tif: Yamaha Corporation; remove backround: Habitator terrae. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_Saxophone_YAS-62.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AYamaha_Saxophone_YAS-62.png&oldid=926933807).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_clarinet.png

File:Yamaha Clarinet YCL-457II-22.tiff: Yamaha Corporation; remove backround: Habitator terrae. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_Clarinet_YCL-457II-22.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AYamaha_Clarinet_YCL-457II-22.png&oldid=1212935488).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_bassoon.png

Yamaha Corporation. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_Bassoon_YFG-812_II.tif). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AYamaha_Bassoon_YFG-812_II.tif&oldid=824627482).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_oboe.png

man ichihara ; Photographed and edited by fi:Käyttäjä:Aquazer. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Oboe.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AOboe.png&oldid=946967623).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_trumpet.png

PJ, background cropped by EWikist. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Trumpet_1.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ATrumpet_1.png&oldid=1125647411).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_trombone.png

Debbie Rawson (photograph); Jonathanischoice (uploader); funded by Wikimedia Aotearoa New Zealand. [CC BY 4.0](https://creativecommons.org/licenses/by/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:WANZ_Instrument_2024-09_IMG_7771_tenor_trombone.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AWANZ_Instrument_2024-09_IMG_7771_tenor_trombone.png&oldid=1185831342).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_tuba.png

Buffet Crampon. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Tuba_Melton_Meinl_Weston.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ATuba_Melton_Meinl_Weston.png&oldid=1142043340).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_harpsichord.png

FREYmanagement. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:Harpsichord_VitalJulianFrey.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AHarpsichord_VitalJulianFrey.jpg&oldid=1269653743).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_vibraphone.png

Xylosmygame at English Wikipedia. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:Soprano_vibraphone_(from_Emil_Richards_Collection).jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ASoprano_vibraphone_%28from_Emil_Richards_Collection%29.jpg&oldid=1148324838).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_glockenspiel.png

flamurai. [Public domain](https://creativecommons.org/publicdomain/mark/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Glockenspiel-malletech.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AGlockenspiel-malletech.jpg&oldid=1239179923).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_steelpan.png

J dahlia. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Steel_Pan.png). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3ASteel_Pan.png&oldid=506232778).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_handpan.png

Pensierarte. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:Handpan.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AHandpan.jpg&oldid=709962177).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_koto.png

Gotō Teijō / Gotō Yūjō. [CC0-1.0](https://creativecommons.org/publicdomain/zero/1.0/).

[Original source](https://commons.wikimedia.org/wiki/File:Koto_MET_DP148476.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AKoto_MET_DP148476.jpg&oldid=1153348730).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_tubular_bells.png

Xylosmygame at English Wikipedia. [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0).

[Original source](https://commons.wikimedia.org/wiki/File:Yamaha_Deagan_chimes_(from_LA_Percussion_Rentals).jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3AYamaha_Deagan_chimes_%28from_LA_Percussion_Rentals%29.jpg&oldid=1231982576).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.

## sfz_handbell.png

M0tty. [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0).

[Original source](https://commons.wikimedia.org/wiki/File:Petite_cloche_manuelle.jpg). [Recorded source revision](https://commons.wikimedia.org/w/index.php?title=File%3APetite_cloche_manuelle.jpg&oldid=1053990107).

Conventional masking, background removal, framing, rotation or recoloring as recorded in assets/sfz-instruments/credits.json; cropped and resized. No AI generation.
