# Multi-player Word Guessing Game
CSC209 Assignment 4 - A multiplayer word guessing game where each player connects via nc and takes turns guessing letters of a predefined word

### Setting up the Project
Clone the git repository and run 'make'

### Running the Server
Run the wordsrv executable with the desired PORT (which can be changed in the Makefile)

### Connecting to the Server
Using a termial, connect using 'nc -c SERVER_NAME PORT'
* Server name is either the external IP address of the server or "localhost" if running on a local machine

### Playing the Game
When it is your turn, simple type a valid character and press enter to guess. The server handles invalid inputs and client disconnects.
