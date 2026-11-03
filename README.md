# Cinnamon preprocessor

Converts GameMaker `data.win` or `game.unx` assets for the Cinnamon 3DS and Wii
targets.

## Build and run

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel

build/native/cinnamon-preprocessor /path/to/game --target target --output output
```

Use `--help` for conversion options.

