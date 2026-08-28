import os

cmds_dir = r"h:\repositories\backup repository\pseuDOS-backup\rewrite-05\pseuDOS\protected\crit\commands"

for filename in os.listdir(cmds_dir):
    if filename.endswith(".py"):
        filepath = os.path.join(cmds_dir, filename)
        with open(filepath, "r", encoding="utf-8") as f:
            lines = f.readlines()
        
        new_lines = []
        for line in lines:
            if "from krnl.essential import kernel" in line:
                continue
            new_lines.append(line)
            
        with open(filepath, "w", encoding="utf-8") as f:
            f.writelines(new_lines)

print("Done removing imports.")
