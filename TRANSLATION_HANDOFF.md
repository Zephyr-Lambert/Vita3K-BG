# Bullet Girls translation: handoff

Starting notes for the translation project, split off from the emulator work.

## The game

- **Title:** Bullet Girls (バレットガールズ), PS Vita, Japan only
- **Title ID:** PCSG00361
- **Goal:** an English translation of the story, menus and UI, released as a patch

## Ground rules

- **Content limit:** sexualized lines involving the characters are not translated. They are
  skipped and flagged in the script for the patch author to decide on, and everything else is
  translated normally. This was agreed at the start.
- **Tools are fine to build:** extractors, dumpers, reinserters, and anything else needed to get
  text and graphics out of the game and back in.

## Plans for the patch so far

1. **Translation:** story, menus and UI text, plus any text baked into graphics.
2. **Stage 1 tree sorting fix:** facing the end of stage 1, the left row of trees draws its
   foliage in the wrong order, so a tree behind shows through the one in front. This happens on
   real hardware too, so it's the game's own data, not an emulator bug. All the foliage on that
   row is one blended draw with depth writes off, with its triangles ordered for looking the other
   way down the street. Fix it in the stage's model data by reordering that mesh's triangles or
   splitting it into one draw per tree, once the model format is understood.
3. **Optional Western button layout:** swap confirm and back (Circle/Cross) in the game's code,
   and swap the button prompt graphics to match. Ship it as a separate optional part of the patch,
   so players can keep the original layout. Vita3K's own confirm-button setting won't do this,
   since the game has Circle as confirm built in.

## Emulator for testing

- **Build:** Vita3K-BG, an unofficial Vita3K fork with fixes for this game.
- **Repo:** `Zephyr-Lambert/Vita3K-BG`, branch `bullet-girls-fixes`.
- **Fixed so far:** audio cutting out and the hang at mission end, OpenGL shadow copies,
  triangular cuts in walls and floors, Vulkan stage textures, the Vulkan bloom trail, and smooth
  bloom when upscaling.
- **Details:** see `README.md` and `BULLET_GIRLS_NOTES.md` in the repo.
- **Save bug (November 2026):** the official Vita3K changed how save slots are stored, and
  saving currently fails with a "not enough free space" popup. This is waiting on the official
  fix; use the build before the merge (`vita3k-4a2a5e9f`) for testing until then.

## First step

Work out how the game stores its files: text, fonts, graphics and models. Start from a listing
of the game's folders and files (names and sizes) before uploading anything large. Find the text
format and font first, since those decide how the translated script goes back in.
