# Localization

The player defaults to English and loads optional UTF-8 language files from the `languages` folder.

Included packs:

```text
en-US.lang
pt-BR.lang
```

## Adding a language

Copy an existing file and rename it with a locale-style code, for example:

```text
es-ES.lang
fr-FR.lang
de-DE.lang
```

Each file is a simple `key=value` UTF-8 document:

```ini
meta.name=Español
menu.file=Archivo
menu.open=Abrir vídeo...
button.play=Reproducir
```

`meta.name` is the human-readable name shown in the Language menu.

Missing keys automatically fall back to the built-in English strings.

The selected language is stored in `DLSSVideoPlayer.ini` under:

```ini
[General]
Language=pt-BR
```

Image-adjustment settings are stored in a separate `[VideoAdjustments]` section, so changing language does not erase video preferences.
