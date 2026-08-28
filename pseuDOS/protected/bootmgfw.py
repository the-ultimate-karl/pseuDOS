import sys
sys.stdout.write("\n")
sys.stdout.write("[bootmgfw] looking for kernel...\n\n")
sys.stdout.write("====================================\n\n")
try:
    from krnl.essential import kernel
except Exception as e:
    sys.stdout.write("            [bootmgfw]\n\n")
    from bootmgr import errtext
    errtext.error_boot(e)
 