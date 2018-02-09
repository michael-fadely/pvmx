# pvmx

This program can be used to create or extract PVMX archives for the [SADX Mod Loader](https://github.com/sonicretro/sadx-mod-loader)'s Texture Pack system.

## Usage
```
-c, --create     Create an archive using the given texture pack index.  
-e, --extract    Extract an archive.
-o, --output     Output file for creation or output directory for extraction.
```

### Example
To create a PVMX from a texture pack folder:
```
pvmx -c "C:\some path"
```

To extract an existing PVMX archive:
```
pvmx -e "some archive.pvmx" -o "C:\put\the\textures\here"
```

If `-o` is not specified, the textures will be put in a folder with the same name as the archive. In the above example, if `-o` was ommitted, a folder called "some archive" would be created to contain the textures.
