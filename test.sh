#!/bin/bash

# Number of clients to run
NUM_CLIENTS=10

# Loop to execute the client program multiple times
for ((i=1; i<=NUM_CLIENTS; i++))
do
    # Generate a random float between 0 and 100
    RANDOM_SEED=$RANDOM  # Get a unique random seed for each iteration
    RANDOM_INPUT=$(awk -v min=0 -v max=100 -v seed=$RANDOM_SEED 'BEGIN{srand(seed); print min+rand()*(max-min)}')
    # Execute the client program with the client ID and pass random input to stdin
    echo "Starting client $i with input $RANDOM_INPUT"
    echo "$RANDOM_INPUT" | ./client "$i" &
done

# Wait for all background processes to finish
wait

echo "All clients have been executed."