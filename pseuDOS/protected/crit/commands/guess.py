__description__ = "play a number guessing game"
__example__ = "guess"

import random

def main():
    kernel = globals().get('kernel')
    
    number = random.randint(1, 100)
    kernel.syscall(1, 1, "I'm thinking of a number between 1 and 100.\n")
    
    attempts = 0
    while True:
        kernel.syscall(1, 1, "Enter your guess: ")
        
        # Read input from fd 0 (stdin)
        guess_str = kernel.syscall(0, 0, 1024)
        if not guess_str:
            break
            
        guess_str = guess_str.strip()
        if not guess_str:
            continue
            
        try:
            guess = int(guess_str)
        except ValueError:
            kernel.syscall(1, 1, "Please enter a valid number.\n")
            continue
            
        attempts += 1
        
        if guess < number:
            kernel.syscall(1, 1, "Too low!\n")
        elif guess > number:
            kernel.syscall(1, 1, "Too high!\n")
        else:
            kernel.syscall(1, 1, f"Congratulations! You guessed it in {attempts} attempts!\n")
            break
