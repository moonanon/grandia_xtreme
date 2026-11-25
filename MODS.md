# Mods

Optional changes to the game, built from your own disc (nothing of the game's
is in this repository) and applied by the port from `mods/`. Without `mods/`
the game is as it was.

```sh
python3 port/tools/mods.py "iso/Grandia Xtreme (USA).iso"   # builds them all into mods/
python3 port/tools/mods.py --list                           # lists them
python3 port/tools/mods.py ISO --without thief-gear         # all but one
```

`mods/mods.txt` lists the ones built. Item and skill mods write one file,
`mods/grxsys.grxp`, which holds only the bytes of `SYS/GRXSYS.PAC` that they
change; their numbers are the tables at the top of `port/tools/grxsys.py`.

## Thief mods

`thief-gear` and `guaranteed-drops` are two takes on the same problem: only
Titto's Get Item steals reliably. **Use one or the other.** They can be built
together, but it makes no sense: `guaranteed-drops` already drops everything, so
the gear's bonus is wasted, and the gear is what keeps Titto from being made
pointless.

### `thief-gear` (the vanilla take)

The more vanilla, "canon" take: it keeps the game's own rules and scale and
gives the other characters thief gear, so Titto is still the thief, only no
longer the only one.

| Item | For | Drop rate | Where |
| --- | --- | --- | --- |
| Thief's Sword (the game's) | Evan, Brandol (was Brandol only) | +10 | reward, as before |
| Brigand's Sword | Evan, Brandol | +20 | their weapon shops, mid-game |
| Corsair's Sword | Evan, Brandol | +30 | their weapon shops, late-game |
| Laverna's Dagger (the game's) | Lutina | +10 | reward, as before |
| Mercury Knife | Lutina | +20 | her weapon shop (same materials as the weapon it copies) |
| Cacus Dagger | Lutina | +30 | her weapon shop (same) |
| Magpie / Plunder / Fortuna Ring | everyone but Titto | +10 / +15 / +20 | accessory shop |
| Mogay Ring / DX / Premium | Titto | +20 / +45 / +70 | accessory shop |

A new weapon's attack is that of the shop weapon it is modelled on, less 12%
(+20) or 17% (+30): the drop rate costs attack. Add the Pirate's Ring skill's +10
at level 5: Titto, fully geared, reaches 100% (a sure drop), the others 60%. The text says how much a piece
raises the drop rate (slightly, moderately, greatly, massively). Nothing else
changes: Get Item, enemies and the skills are as they were, and Titto stays ahead.

### `guaranteed-drops` (the hard take)

A deliberate overhaul, not a canon one: it makes stealing a sure thing for
anyone with the skills.

Pirate's Ring and Thief Trick give +20% a level instead of +2%: at level 5,
every enemy that has an item drops it (Pirate's Ring), and the best one it can
(Thief Trick, which picks the top row of its drop table). The Pirate's Ring text
said "more gold", though it raises the item chance; it now says what it does.
No gear is added: anyone can learn the skills.

An enemy that rolled no item at the start of the battle still drops nothing.

## `bandit-gear`

The gold line, beside `thief-gear`'s item line; either works without the other.
When an enemy drops gold, it is its small payout or its large one (three times
the small, on those that have both). Gold gear raises the chance of the large
one by the **killer's** bonus. It doesn't change the amounts, and an enemy that
drops its item gives no gold. Enemies with only one payout, many bosses among
them, are unaffected.

| Armor | Gold bonus | Defense | Price | Where |
| --- | --- | --- | --- | --- |
| Bandit Armor (the game's) | +10 | 18 | | not sold |
| Highwayman's Garb | +20 | 56 (Aura Armor's 64, less 12%) | 9200 | armor shop, with Aura Armor |
| Lupin's Vest | +30 | 73 (Dark Armor's 88, less 17%) | 20000 | armor shop, with Dark Armor |

Evan, Brandol, Carmyne, Titto, Ulk and Lutina can wear them, as Bandit Armor.
The new pieces keep none of the copied armor's other bonuses or effects. They
stack with the game's Geo Blade and Miser Turban (+10 each).

The game uses one roll for both drops, so item-rate gear takes the high rolls
that would have given large gold: on an enemy with an item, enough of it cancels
the gold bonus. Gold gear never lowers the item chance.

## Geo Gates and the map

| Mod | What it does |
| --- | --- |
| `land-ruins-2-geo-gate` | a new Geo Gate on Land Ruins 2, which has none |
| `land-ruins-3-geo-gate` | Land Ruins 3's exit-only Geo Gate made two-way |
| `aqua-ruins-geo-gate` | the Energy Conversion area's exit-only Geo Gate made two-way |
| `gale-ruins-3-geo-gate` | a new Geo Gate deeper in Gale Ruins |
| `map-overlay` | M (or R3) spreads the minimap flat over the whole field while you keep walking |
| `minimap-entrances` | the minimap shows every map's entrances, and Geo Gates in cyan |

The Geo Gate mods patch map files (`mods/mdt/XXXX.mdp`, named after the map
file, four for a map's story variants) and Locca's gate list (`mods/flaglist.txt`).
A gate is activated as the game's are: touch it once, then again; it then shows in
Locca's list. `GATES` in `port/tools/mods.py` is where to add more.
