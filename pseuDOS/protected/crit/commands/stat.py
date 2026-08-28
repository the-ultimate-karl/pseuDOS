__description__ = "get file status"
__example__ = "stat file.txt"

def main():
    boot = kernel
    if not args:
        boot.syscall(1, 1, "usage: stat <file>\n")
        return
        
    path = args[0]
    st = boot.syscall(4, path) # sys_stat
    
    if st == -1 or st is None:
        boot.syscall(1, 1, f"{RED}stat: cannot stat '{path}': No such file or directory{RESET}\n")
        return
        
    type_str = "directory" if st.get("is_dir") else "file"
    size = st.get("size", 0)
    
    boot.syscall(1, 1, f"  {BOLD}{BLUE}File:{RESET} {path}\n")
    boot.syscall(1, 1, f"  {BOLD}{GREEN}Size:{RESET} {size} bytes\n")
    boot.syscall(1, 1, f"  {BOLD}{CYAN}Type:{RESET} {type_str}\n")

if __name__ == "__main__":
    main()
