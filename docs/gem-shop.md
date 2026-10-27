# Premium plants for gems

Offline, the 4.5.2 shop cannot sell the real-money premium plants. It shows
"service unavailable". This fork's loader changes the shop catalog so those
plants cost gems instead. Gems still have to be earned in the game, and saves
are never edited.

Code: `vita/direct/source/utils/gem_shop.c`. It runs at boot, before anything
reads the OBB. The CMake option `PVZ2_GEM_SHOP` is ON by default.

## What changes

The catalog is `PACKAGES\MAGENTO.RTON` in the `Packages` resource group. For
every `MagentoProductProps` object with `ObjectType: "plant"` and a plant in the
table below:

```
"Prices": [{"Price": 2.99}]   ->   "PriceGems": "35", "Prices": [{"Price": 0}]
```

Any existing `PriceGems` key is replaced. On the stock 4.5.2 ROW OBB this
rewrites 170 product entries (each plant has several store variants).

| Gems | Plants |
| --: | :-- |
| 35 | imitater, jalapeno, peanut, powerlily, squash |
| 45 | chomper, hypnoshroom, sapfling, snowpea, starfruit, torchwood, hurrikale |
| 55 | toadstool, strawburst, cactus, firepeashooter, ghostpepper, homingthistle, sweetpotato, lavaguava |
| 75 | electricblueberry, jackolantern, grapeshot, coldsnapdragon, escaperoot, shrinkingviolet, bloominghearts |

Prices are about 11 gems per dollar, matching the store's 110 gems = $9.99 pack.

## How the OBB is patched

1. Unpack the `Packages` group and decode the catalog. The decoded catalog
   must re-encode to exactly the original bytes. Otherwise nothing is changed.
2. Apply the price change, then put the new catalog on a fresh 4 KiB page at
   the end of the unpacked group and repoint its file entry.
3. Recompress the group and append it after the end of the OBB (about 600 KB).
   The old group stays where it was, untouched.
4. Save the original header bytes and file size to
   `ux0:data/pvz2/userdata/gem_shop.state`.
5. Repoint three header records (28 bytes) to the new group: the group's
   offset/size, its data sizes, and its pool size.

If power is lost between steps 3 and 5, the next launch finishes the change,
or undoes it if the feature is off. If a different OBB is installed, the old
state file is discarded and the new OBB is checked again. If an OBB already
sells these plants for gems (for example the earlier gem-shop OBB mod), it is
left alone.

## Turning it off

Create an empty file `ux0:data/pvz2/userdata/gem_shop_off.txt` and launch.
The loader writes back the original header bytes and trims the appended data,
so the OBB is byte-identical to the original again. Delete the file to turn the
change back on.

## Testing

- Host build of `gem_shop.c` against the stock OBB: the new catalog is
  byte-identical to the Python reference tool's output. The other 723 files in
  the group are unchanged, and only header bytes differ before the appended
  data.
- Re-running does nothing. Turning it off restores a file with the same SHA-1
  as the original. Interrupted runs are finished or undone. A replaced OBB is
  patched again. The gem-shop OBB mod is detected and left alone.
- On a PS Vita (2026-10-27): the stock OBB was patched at first launch in the
  same way as on the PC (same new file size). The game booted normally, and every
  premium plant in the shop showed as purchasable with gems.
