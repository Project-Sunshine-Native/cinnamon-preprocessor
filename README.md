# Cinnamon preprocessor

Converts GameMaker `data.win` or `game.unx` assets for the Cinnamon 3DS and Wii
targets.

## Build and run

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --parallel

build/native/cinnamon-preprocessor /path/to/game --target target --output output
```

For 3DS, set the target to ```3ds``` and your output to the SD Card's root. the program will copy all needed files to your apps directory under ```sdmc:3ds/cinnamon/<game>```

Use `--help` for conversion options.

