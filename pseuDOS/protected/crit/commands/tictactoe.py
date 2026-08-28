__description__ = "play a game of tic-tac-toe against an AI"
__example__ = "tictactoe"

def print_board(kernel, board):
    kernel.syscall(1, 1, f" {board[0]} | {board[1]} | {board[2]} \n")
    kernel.syscall(1, 1, "---+---+---\n")
    kernel.syscall(1, 1, f" {board[3]} | {board[4]} | {board[5]} \n")
    kernel.syscall(1, 1, "---+---+---\n")
    kernel.syscall(1, 1, f" {board[6]} | {board[7]} | {board[8]} \n")

def check_win(board, player):
    win_cond = [(0,1,2), (3,4,5), (6,7,8), (0,3,6), (1,4,7), (2,5,8), (0,4,8), (2,4,6)]
    return any(board[a] == board[b] == board[c] == player for a, b, c in win_cond)

def minimax(board, is_maximizing):
    if check_win(board, 'O'): return 1
    if check_win(board, 'X'): return -1
    if all(str(x) in ['X', 'O'] for x in board): return 0
    
    if is_maximizing:
        best_score = -100
        for i in range(9):
            if board[i] not in ['X', 'O']:
                temp = board[i]
                board[i] = 'O'
                score = minimax(board, False)
                board[i] = temp
                if score > best_score:
                    best_score = score
        return best_score
    else:
        best_score = 100
        for i in range(9):
            if board[i] not in ['X', 'O']:
                temp = board[i]
                board[i] = 'X'
                score = minimax(board, True)
                board[i] = temp
                if score < best_score:
                    best_score = score
        return best_score

def get_best_move(board):
    best_score = -100
    best_move = 0
    for i in range(9):
        if board[i] not in ['X', 'O']:
            temp = board[i]
            board[i] = 'O'
            score = minimax(board, False)
            board[i] = temp
            if score > best_score:
                best_score = score
                best_move = i
    return best_move

def main():
    kernel = globals().get('kernel')
    
    board = ['1', '2', '3', '4', '5', '6', '7', '8', '9']
    current_player = 'X'
    
    while True:
        if current_player == 'X':
            print_board(kernel, board)
            kernel.syscall(1, 1, f"Player {current_player}, choose a position (1-9): ")
            
            pos_str = kernel.syscall(0, 0, 1024)
            if not pos_str:
                break
                
            pos_str = pos_str.strip()
            try:
                pos = int(pos_str) - 1
                if pos < 0 or pos > 8 or board[pos] in ['X', 'O']:
                    kernel.syscall(1, 1, "Invalid move.\n")
                    continue
            except ValueError:
                kernel.syscall(1, 1, "Please enter a valid number.\n")
                continue
        else:
            kernel.syscall(1, 1, "AI is thinking...\n")
            pos = get_best_move(board)
            
        board[pos] = current_player
        
        if check_win(board, current_player):
            print_board(kernel, board)
            if current_player == 'O':
                kernel.syscall(1, 1, "AI wins!\n")
            else:
                kernel.syscall(1, 1, "You win!\n")
            break
            
        if all(str(x) in ['X', 'O'] for x in board):
            print_board(kernel, board)
            kernel.syscall(1, 1, "It's a draw!\n")
            break
            
        current_player = 'O' if current_player == 'X' else 'X'
