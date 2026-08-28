__description__ = "play rock, paper, scissors"
__example__ = "rps"

import random

def main():
    kernel = globals().get('kernel')
    
    choices = ['rock', 'paper', 'scissors']
    
    while True:
        kernel.syscall(1, 1, "Enter rock, paper, scissors (or quit): ")
        user_choice = kernel.syscall(0, 0, 1024)
        
        if not user_choice:
            break
            
        user_choice = user_choice.strip().lower()
        if user_choice == 'quit':
            break
            
        if user_choice not in choices:
            kernel.syscall(1, 1, "Invalid choice.\n")
            continue
            
        ai_choice = random.choice(choices)
        kernel.syscall(1, 1, f"AI chose: {ai_choice}\n")
        
        if user_choice == ai_choice:
            kernel.syscall(1, 1, "It's a tie!\n")
        elif (user_choice == 'rock' and ai_choice == 'scissors') or \
             (user_choice == 'paper' and ai_choice == 'rock') or \
             (user_choice == 'scissors' and ai_choice == 'paper'):
            kernel.syscall(1, 1, "You win!\n")
        else:
            kernel.syscall(1, 1, "You lose!\n")
