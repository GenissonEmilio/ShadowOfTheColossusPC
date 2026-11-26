"""Launcher preferences and translations independent of the PS2 disc locale."""
from functools import lru_cache
import json
import locale
from pathlib import Path
import sys


LANGUAGES = {"pt": "Português", "en": "English", "es": "Español", "ja": "日本語"}
PROMPTS = ("auto", "playstation", "xbox", "keyboard")
active = "en"
PREFERENCES = "launcher-preferences.json"


def preferred_language():
    system, _ = locale.getlocale()
    language = system.split("_", 1)[0].lower() if system else "en"
    return language if language in LANGUAGES else "en"


def activate(language):
    global active
    active = language if language in LANGUAGES else preferred_language()


def preferences(directory):
    try:
        config = json.loads((directory / PREFERENCES).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        config = {}
    if not isinstance(config, dict):
        config = {}
    language = config.get("language")
    prompts = config.get("button_prompts")
    campaign = config.get("campaign_languages")
    return {"language": language if isinstance(language, str) and language in LANGUAGES else preferred_language(),
            "campaign_languages": campaign if isinstance(campaign, dict) else {},
            "button_prompts": prompts if prompts in PROMPTS else PROMPTS[0],
            "infinite_health": config.get("infinite_health") is True,
            "infinite_grip": config.get("infinite_grip") is True,
            "launcher_fullscreen": config.get("launcher_fullscreen") is True,
            "ui_sounds": config.get("ui_sounds") is not False}


@lru_cache(maxsize=1)
def catalog():
    bundle = Path(getattr(sys, "_MEIPASS", Path(__file__).parent))
    path = bundle / "assets/launcher-languages.tsv"
    messages = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            source, english, spanish, japanese = line.replace(r"\n", "\n").split("\t")
            if source in messages:
                raise ValueError(f"Duplicate launcher translation: {source}")
            messages[source] = {"en": english, "es": spanish, "ja": japanese}
    return messages


def translate(text):
    return text if active == "pt" else catalog().get(text, {}).get(active, text)
