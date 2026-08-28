import os

cmds_dir = r"h:\repositories\backup repository\pseuDOS-backup\rewrite-05\pseuDOS\protected\crit\commands"

for filename in os.listdir(cmds_dir):
    if filename.endswith(".py"):
        filepath = os.path.join(cmds_dir, filename)
        with open(filepath, "r", encoding="utf-8") as f:
            content = f.read()
        
        # Replace the broken block
        broken_block1 = "    if 'kernel' in globals():\n        boot = kernel\n    else:\n        \n"
        broken_block2 = "    if 'kernel' in globals():\n        boot = kernel\n    else:\n"
        broken_block3 = "    if 'kernel' in globals():\n        boot = kernel\n"
        
        if broken_block1 in content:
            content = content.replace(broken_block1, "    boot = kernel\n")
        elif broken_block2 in content:
            content = content.replace(broken_block2, "    boot = kernel\n")
        elif broken_block3 in content:
            content = content.replace(broken_block3, "    boot = kernel\n")
            
        with open(filepath, "w", encoding="utf-8") as f:
            f.write(content)

print("Done fixing syntax errors.")
