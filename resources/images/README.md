# Launcher images

The logo is five layers with the same canvas, drawn into one rectangle (trilinear filtered), in this order:

- `kana.png`: the flat turquoise kana behind everything, shifted a little with the mouse.
- `logo.png`: GOD HAND and its shadow. It stays still. Without it the launcher writes the name in text.
- `rearmed.png`: the REARMED plate without its letters.
- `rearmed_text.png`: the plate's letters, white with a slight shade; the launcher tints them gold.
- `rearmed_glow.png`: the letters blurred, white; the hot glow when Play is pressed (the plate then plays the game's
  roulette pick: the letters heat up and bloom, a yellow bar shoots out both ways and thins to streaks, while the
  rest fades to black).

They come from K's layered GHR_logo.psd through `tools/logo/export_launcher_logo.py --psd GHR_logo.psd --readme
assets/logo.png`, which also prints `kLogoCentreX/Y` (GOD HAND's centre) and `kRearmedRect` (the plate) for
src/kamite/launcher.cpp. GOD HAND itself is rendered by `tools/logo/render_wordmark.py` from K's letter SVG and his
hand-painted crack PSD; K's own effects pass is on top of that.

`burst.png`: the speed lines behind the logo (square, centred; turquoise with the line strength in alpha, fading to
nothing at the circle's edge). The launcher draws it twice, turning and pulsing, squashed to 3/4 height, centred on
GOD HAND (`kLogoCentreX/Y`), and shifts it with the mouse.

The launcher's background is the saloon (`resources/saloon`, src/kamite/launcher_saloon.cpp): the logo sits in its
doorway's glare (placed by GOD HAND + plate's box in the canvas, riding the Options zoom), the burst behind it sized by
the logo. Without the saloon (no pack, no AVIF decoder) the launcher draws its brown gradient and the logo at the top.

`tools/logo/build_logo.py` built the old KAMITE logo (wordmark + kana over it); kept for its burst.png step. The whole
`resources/` folder is copied next to GHR.exe at build time.

`steam_logo.png`: the five logo layers flattened (REARMED's letters in the launcher's gold), 1280 px wide: the Steam
library logo Add to Steam writes (src/kamite/launcher_steam.cpp). Rebuild it the same way when the layers change.

`steam_icon.png`: src/kamite/ghr.ico's 256 px frame as a PNG, the Steam entry's icon (Steam reads the exe's
PNG-compressed icon frames with a black background).

`steam_cover.jpg`: the Steam entry's 600x900 cover (masterlynx's art with our logo, used with the artist's permission),
scaled from K's full-size file; Add to Steam writes it as the library cover.

`steam_mark.png`: the bill's Add to Steam mark: a sketched Steam icon from Flaticon (flaticon.com, free with
attribution: README credits) in a hand-inked ring (`tools/launcher/steam_mark.py`), white with the ink in alpha; the
bill inks it through its perspective.

`cursor_hand.png`: K's hand, GHR's mouse pointer everywhere, the launcher included (src/kamite/menu_mouse.cpp install,
runtime ps2_host_cursor: a GLFW cursor; hot spot at the index fingertip, (80, 8) of 474 x 556; 0.055 of the window's
height).
