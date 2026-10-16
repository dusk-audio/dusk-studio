# Artwork decoder fixtures

`overexpanded-idat.png` declares one 8-bit RGBA pixel but its IDAT expands to
32 MiB of zero bytes. Only five decoded bytes are needed for the declared row.
The PNG chunks and zlib stream have valid checksums. This exercises inflation
beyond the declared image dimensions, rather than an early header rejection.

The fixture was generated locally with Python's `struct` and zlib 1.3.1,
using compression level 9 and 512 chunks of 65,536 zero bytes. Its IHDR is
`struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)`; IDAT contains the resulting
zlib stream and IEND is empty. Each chunk carries its standard CRC-32.

SHA-256: `d66fa204f7dea085b664c8254c08f9b054df0ee788a6685137beccb9504d9201`.

It contains no source artwork or third-party content.
