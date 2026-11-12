#!/usr/bin/env python3
"""Prepare le dossier webtool/firmware/ servi par le flasher navigateur.

Le flasher charge un manifeste JSON, puis les trois images qu'il annonce.
Ce script copie les binaires produits par idf.py build dans deux dossiers :

  bin/               les trois .bin et SHA256SUMS, pour un flash par esptool
  webtool/firmware/  les memes images plus manifest.json, pour le flasher
                     navigateur, qui resout firmware/ relativement a la page

Les deux jeux sont ecrits dans la meme passe a partir du meme build, donc ils
ne peuvent pas diverger. Les adresses et les parametres memoire ne viennent pas
de flasher_args.json : ils sont imposes ici, identiques a ceux que la page
refuse de laisser changer.

Usage :
    python3 tools/package_webtool_firmware.py [--build build]

Idempotent. N'ecrit jamais dans le dossier de build.
"""

import argparse
import hashlib
import json
import re
import shutil
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
DEST = RACINE / "webtool" / "firmware"
BIN = RACINE / "bin"
PUCE = "esp32s3"
CHIP_FAMILY = "ESP32-S3"
PARAMETRES = {"flashMode": "dio", "flashFreq": "80m", "flashSize": "16MB"}

# (cle, nom public, chemin dans le build, adresse)
IMAGES = [
    ("bootloader", "bootloader.bin", "bootloader/bootloader.bin", 0x0),
    ("partition-table", "partition-table.bin", "partition_table/partition-table.bin", 0x8000),
    ("rfsentinel", "rfsentinel.bin", "rfsentinel.bin", 0x10000),
]


def sha256(chemin):
    h = hashlib.sha256()
    with chemin.open("rb") as f:
        for bloc in iter(lambda: f.read(1 << 20), b""):
            h.update(bloc)
    return h.hexdigest()


def md5(chemin):
    h = hashlib.md5()
    with chemin.open("rb") as f:
        for bloc in iter(lambda: f.read(1 << 20), b""):
            h.update(bloc)
    return h.hexdigest()


def version():
    entete = RACINE / "main" / "version.h"
    texte = entete.read_text(encoding="utf-8")
    m = re.search('define +RFS_VERSION +"([^"]+)"', texte)
    if not m:
        raise SystemExit("RFS_VERSION introuvable dans %s" % entete)
    return m.group(1)


def controler_build(build):
    manquants = [str(build / rel) for _, _, rel, _ in IMAGES if not (build / rel).is_file()]
    if manquants:
        raise SystemExit(
            "images absentes du build :\n  " + "\n  ".join(manquants) +
            "\nLancer d'abord : idf.py build"
        )
    args = build / "flasher_args.json"
    if args.is_file():
        reglages = json.loads(args.read_text(encoding="utf-8"))
        attendu = {"flash_mode": PARAMETRES["flashMode"], "flash_size": PARAMETRES["flashSize"],
                   "flash_freq": PARAMETRES["flashFreq"]}
        reel = reglages.get("flash_settings", {})
        if reel and reel != attendu:
            raise SystemExit(
                "le build annonce %s alors que le flasher impose %s : "
                "corriger la table de partitions ou le menuconfig" % (reel, attendu)
            )
        puce = reglages.get("extra_esptool_args", {}).get("chip")
        if puce and puce != PUCE:
            raise SystemExit("le build est pour %s, pas pour %s" % (puce, PUCE))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=str(RACINE / "build"), help="dossier de build idf.py")
    args = ap.parse_args()
    build = Path(args.build)
    controler_build(build)

    DEST.mkdir(parents=True, exist_ok=True)
    BIN.mkdir(parents=True, exist_ok=True)
    entete = version()
    parties = []
    empreintes = []
    for cle, nom, rel, adresse in IMAGES:
        source = build / rel
        copie = DEST / nom
        if not copie.exists() or source.stat().st_mtime_ns > copie.stat().st_mtime_ns or (
            copie.stat().st_size != source.stat().st_size
        ):
            shutil.copyfile(source, copie)
        miroir = BIN / nom
        if not miroir.exists() or miroir.stat().st_size != source.stat().st_size or (
            source.stat().st_mtime_ns > miroir.stat().st_mtime_ns
        ):
            shutil.copyfile(source, miroir)
        digest = sha256(copie)
        if sha256(miroir) != digest:
            raise SystemExit("bin/%s ne correspond pas a webtool/firmware/%s" % (nom, nom))
        signature = md5(copie)
        taille = copie.stat().st_size
        parties.append({"id": cle, "name": cle, "file": nom, "path": nom, "offset": adresse,
                        "size": taille, "taille": taille, "sha256": digest, "md5": signature})
        empreintes.append(digest + "  " + nom + chr(10))

    manifeste = {
        "name": "RFSentinel",
        "version": entete,
        "chipFamily": CHIP_FAMILY,
        "chip": PUCE,
        "flashMode": PARAMETRES["flashMode"],
        "flashFreq": PARAMETRES["flashFreq"],
        "flashSize": PARAMETRES["flashSize"],
        "parts": parties,
    }
    (DEST / "manifest.json").write_text(
        json.dumps(manifeste, indent=2, ensure_ascii=False) + chr(10), encoding="utf-8"
    )
    (DEST / "SHA256SUMS").write_text("".join(empreintes), encoding="utf-8")
    (BIN / "SHA256SUMS").write_text("".join(empreintes), encoding="utf-8")

    print("RFSentinel %s -> %s/ et %s/" % (entete, DEST.relative_to(RACINE),
                                           BIN.relative_to(RACINE)))
    for p in parties:
        print("  0x%05X  %-22s %9d octets  sha256 %s" % (p["offset"], p["file"], p["size"],
                                                         p["sha256"][:16]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
