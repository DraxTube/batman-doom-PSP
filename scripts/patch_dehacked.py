#!/usr/bin/env python3
"""Patch d_main.c to call DEH_LoadFromWADs() and DEH_LoadFromFile()
after WAD initialization. PSP version uses ms0:/PSP/GAME/BATDOOM/"""

DATA_PATH = "ms0:/PSP/GAME/BATDOOM/"

with open('d_main.c', 'r') as f:
    content = f.read()

# Add include for our DEH loader
if '#include "deh_loader.h"' not in content:
    content = '#include "deh_loader.h"\n' + content

# Add strings.h for strncasecmp
if '#include <strings.h>' not in content and '#include <string.h>' in content:
    content = content.replace('#include <string.h>', '#include <string.h>\n#include <strings.h>', 1)

# Inject DEH loading calls after W_GenerateHashTable()
target = 'W_GenerateHashTable();'
inject = target + f'''

    // PSP: Load DEHACKED lumps from PWADs
    DEH_LoadFromWADs();

    // PSP: Load external .DEH file (Batman Doom uses BATMAN.DEH)
    DEH_LoadFromFile("{DATA_PATH}batman.deh");
    DEH_LoadFromFile("{DATA_PATH}BATMAN.DEH");
    DEH_LoadFromFile("{DATA_PATH}Batman.deh");'''

if 'DEH_LoadFromWADs' not in content:
    if target in content:
        content = content.replace(target, inject, 1)
        print('Injected DEH_LoadFromWADs() and DEH_LoadFromFile() after W_GenerateHashTable')
    else:
        print('WARNING: W_GenerateHashTable not found')
else:
    print('DEH loading already present')

with open('d_main.c', 'w') as f:
    f.write(content)
print('d_main.c patched successfully')
