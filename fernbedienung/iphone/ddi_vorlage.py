"""Erzeugt ddi_vorlage.plist fuer das iBook aus dem BuildManifest des Entwickler-Abbilds (Cryptex, ab iOS 26.4).
Aufruf: python ddi_vorlage.py <Ordner mit BuildManifest.plist> <Ziel.plist>"""
import plistlib
import sys

ordner, ziel = sys.argv[1], sys.argv[2]
m = plistlib.load(open(ordner + "/BuildManifest.plist", "rb"))
ident = next(b for b in m["BuildIdentities"] if b.get("Info", {}).get("Variant", "").endswith("Developer Disk Image Cryptex"))

tss = {
    "@Cryptex1,Ticket": True,
    "Cryptex1,ChipID": int(str(ident["Cryptex1,ChipID"]), 0),
    "Cryptex1,Type": ident["Cryptex1,Type"],
    "Cryptex1,SubType": ident["Cryptex1,SubType"],
    "Cryptex1,ProductClass": int(str(ident["Cryptex1,ProductClass"]), 0),
    "Cryptex1,UseProductClass": ident["Cryptex1,UseProductClass"],
    "Cryptex1,NonceDomain": ident["Cryptex1,NonceDomain"],
    "Cryptex1,Version": ident["Cryptex1,Version"],
    "Cryptex1,PreauthorizationVersion": ident["Cryptex1,PreauthorizationVersion"],
    "Cryptex1,UniqueTagList": b"",
}
for k, e in ident["Manifest"].items():
    if k.startswith("Cryptex1,") and e.get("Info", {}).get("Personalize", False):
        tss[k] = {"Digest": e["Digest"]}

dateien = {}
for k, feld in (("Cryptex1,GenericDmg", "image"), ("Cryptex1,GenericTrustCache", "trustcache"),
                ("Cryptex1,CryptexInfoPlist", "info"), ("Cryptex1,GenericVolume", "volumehash")):
    dateien[feld] = ident["Manifest"][k]["Info"]["Path"]

vorlage = {
    "Build": m.get("ProductBuildVersion"),
    "TSS": tss,
    "Dateien": dateien,
    "SubType": ident["Cryptex1,SubType"],
    "NonceDomain": ident["Cryptex1,NonceDomain"],
    "UseProductClass": ident["Cryptex1,UseProductClass"],
    "Version": ident["Cryptex1,Version"],
    "PreauthVersion": ident["Cryptex1,PreauthorizationVersion"],
}
plistlib.dump(vorlage, open(ziel, "wb"))
print(vorlage["Build"], dateien)
