# FBX Pipeline — Import/Export Format Compatibility

**Date:** Mar 23, 2026
**Status:** Confirmed via testing

---

## Key Finding

Urho3D's AssetImporter supports two FBX pathways, but they use **incompatible FBX format versions** and each pathway is **one-directional only**.

## The Two Pathways

### Import: Old FBX → Urho

- **FBX version:** Pre-7500 (older Assimp FBX importer)
- **Direction:** FBX → `.mdl` + `.ani` (Urho native formats)
- **Sources:** Mixamo, Quaternius, any external FBX asset
- **Command:** `AssetImporter model input.fbx output.mdl`
- **Cannot** import FBX 7500 files

### Export: Urho → FBX 7500

- **FBX version:** 7500 (upgraded Assimp FBX exporter)
- **Direction:** `.mdl` + `.ani` → FBX 7500
- **Target:** Blender (for editing, re-rigging, adding animations)
- **Cannot** export to old FBX format

## Round-Trip Workflow

```
External Asset (Mixamo, etc.)
    │
    ▼ Old FBX
AssetImporter import
    │
    ▼ .mdl / .ani
Urho3D (ModelViewer, Sample 60, etc.)
    │
    ▼ FBX 7500 (AssetImporter export)
Blender
    │  (edit, add anims, rekey, modify)
    │
    ▼ Old FBX (Blender FBX export)
AssetImporter import
    │
    ▼ .mdl / .ani
Urho3D
```

The round-trip works, but **Blender is the format bridge**. You cannot skip Blender when going from FBX 7500 back to Urho.

## Rules for Developers

1. **Importing into Urho:** Use old FBX files only. FBX 7500 files will not import correctly.
2. **Exporting from Urho:** Produces FBX 7500 only. This is for Blender consumption.
3. **Blender round-trip:** Export from Blender using its FBX exporter (produces old-format FBX compatible with AssetImporter import).
4. **Do not** attempt to feed exported FBX 7500 back into AssetImporter import.
5. **Do not** expect AssetImporter export to produce old-format FBX.

## Why Two Formats

The old Assimp FBX importer and the new Assimp FBX exporter target different FBX specification versions. FBX is a proprietary Autodesk format with no single open standard — different versions have different binary layouts, node structures, and feature sets. Assimp's importer was written against older FBX files (widespread in asset stores), while the exporter targets FBX 7500 (the version Blender reads best).
