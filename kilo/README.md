# Kilo text editor.

A text editor has two logical compartments the ui to display and the actual text processing which involves file loading, editing, searching etc.

Linux terminals have apis to allow user programs to control itself.
That is we can ask from our user program to do the following:
1. Clear screen.
2. Display cursor at a specific coordinate
3. Ask from it terminal states like height and width.
4. Not handle certain user inputs like ctrl+c
5. Pass all keystrokes to the user program
6. Handle newlines in certain ways.

We modify terminal global states by a c struct. We pass information to the terminal runtime by special escape characters. We can also recieve replies from the terminal in this way.

So the above basically means with the right api we can control the screen and the cursor, and get whaterver keyboard inputs the user passes to the program directly without the terminal modifying anything within.

# Render loop
We represent text as a series of string rows. 
On starting the editor we choose to display row 0 to rowHeight-1. Our cursor is rendered at the start of row 0 and column 0, through terminal api say write(\cursor, x, y) 


1. Display cursor at (0,0) then display intial rows
2. Wait for input 
2. On input to move cursor (say to right):
	- Increment our cursor data structure appropriately 
	- Display cursor at (0,1) then display initial rows again
3. Goto 2

So we render the entire screen each time **anything** needs to be changed in the screen which may be newset of rows of text or something as small as a cursor.

# Scrolling (Vertical)
We have a rowOffset which tells us which row of file to start rendering text from whenever we are rendering.
Rows displayed is therefore (rowOffset) to (rowOffset + screenrows) if total number of rows is greater than screenrows.
If we move our cursor down it should not scroll until we move past last row being displayed.
Let us also keep the convention that cy the cursor y position is the cursor to the file rows (not the terminal display rows).
So when cursor is at top left (0th by terminal standard) it should be equal to rowOffset. 
When cursor is moved past lowest bound of terminal, it means it has value greater than (rowOffset+screenrows). Thus we can recognize it by the same condition and adjust rowOffset by the difference of (rowOffset + screenRows) and cy (current row position).

On moving up (if we can it means rowOffset != 0) we can simply update rowOffset with current cy

Note: Since cy/cx is file row/column relative we have to normalize them when displaying them in terminal
