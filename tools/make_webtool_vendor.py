#!/usr/bin/env python3
"""Genere la variante « script classique » du bundle esptool-js.

esptool-js est livre en modules ES (bundle.js). Le flasher RFSentinel doit
fonctionner sans serveur, donc sans import ESM : Chrome bloque les imports
depuis file://. On remplace la seule instruction export finale par une
assignation a un global, sans toucher au reste du code.

Usage :
    python3 tools/make_webtool_vendor.py [--source <bundle.js>]

Le fichier genere est verifie : il doit se parser par node et ne contenir
ni export, ni import dynamique. Aucune expression reguliere ici, pour eviter
les pieges d'echappement.
"""

import argparse
import subprocess
import sys
from pathlib import Path

RACINE = Path(__file__).resolve().parent.parent
VEND = RACINE / "webtool" / "vendor" / "esptool-js"
ORIGINAL = VEND / "bundle.js"
SORTIE = VEND / "bundle.global.js"
GLOBAL_NAME = "esptooljs"
DEBUT_EXPORT = "export{"
FIN_EXPORT = "};"
SAUT = chr(10)


def parse_export(clause):
    """Retourne [(nom_interne, nom_public)] depuis le contenu de export{...}."""
    paires = []
    for entree in clause.split(","):
        entree = entree.strip()
        if not entree:
            continue
        if " as " in entree:
            interne, public = [x.strip() for x in entree.split(" as ", 1)]
        else:
            interne = public = entree
        paires.append((interne, public))
    return paires


def est_identifiant(nom):
    """Vrai si nom est un identifiant JavaScript valide."""
    if not nom:
        return False
    if not (nom[0].isalpha() or nom[0] in "_$"):
        return False
    return all(c.isalnum() or c in "_$" for c in nom)


def construire(texte_original):
    """Transforme le module ESM en script classique. Retourne (texte, paires)."""
    nb = texte_original.count(DEBUT_EXPORT)
    if nb != 1:
        raise SystemExit(
            "bundle.js contient %d instructions export{ : la transformation "
            "n'est prevue que pour une seule" % nb
        )
    if "import(" in texte_original or "import.meta" in texte_original:
        raise SystemExit(
            "bundle.js utilise un import dynamique ou import.meta : impossible "
            "de le charger en script classique"
        )
    debut = texte_original.index(DEBUT_EXPORT)
    tete = texte_original[:debut]
    queue = texte_original[debut + len(DEBUT_EXPORT) :].rstrip()
    if not queue.endswith(FIN_EXPORT):
        raise SystemExit("instruction export finale invalide en fin de fichier")
    clause = queue[: -len(FIN_EXPORT)]
    paires = parse_export(clause)
    if not paires:
        raise SystemExit("liste d'exports vide")
    for interne, public in paires:
        if not (est_identifiant(interne) and est_identifiant(public)):
            raise SystemExit("nom exporte inattendu : %r %r" % (interne, public))
    affect = ",".join(public + ":" + interne for interne, public in paires)
    entete = (
        "/* Genere par tools/make_webtool_vendor.py a partir de bundle.js "
        "(esptool-js, Apache-2.0). Ne pas editer a la main. Export ESM "
        "remplace par globalThis." + GLOBAL_NAME + ". */"
    )
    sortie = entete + SAUT + tete + SAUT + "globalThis." + GLOBAL_NAME + "={" + affect + "};" + SAUT
    return sortie, paires


def verifier():
    """Le fichier genere doit se parser par node et rester un script classique."""
    texte = SORTIE.read_text(encoding="utf-8")
    if DEBUT_EXPORT in texte:
        raise SystemExit("le fichier genere contient encore un export")
    if "import(" in texte or "import.meta" in texte:
        raise SystemExit("le fichier genere contient un import dynamique")
    subprocess.run(
        ["node", "--check", str(SORTIE)],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.STDOUT,
    )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default=str(ORIGINAL), help="bundle.js d'origine")
    args = ap.parse_args()
    src = Path(args.source)
    if not src.exists():
        raise SystemExit("source introuvable : %s" % src)
    texte, paires = construire(src.read_text(encoding="utf-8"))
    SORTIE.write_text(texte, encoding="utf-8")
    verifier()
    print(
        "%s : %d exports, %d octets"
        % (SORTIE.relative_to(RACINE), len(paires), SORTIE.stat().st_size)
    )
    print("noms : " + ", ".join(sorted(p for _, p in paires)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
