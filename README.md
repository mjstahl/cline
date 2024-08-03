# Common Lisp mINimal Editor

This begins as a reimplementation of antirez's [Kilo](https://github.com/antirez/kilo).

This project is for personal education and not for public consumption.

The education I hope to derive from this project is:
1. Understand how terminals render text to the screen.
2. Get a better grasp on how editors are constructed and the data structures used to build them.
3. Write Common Lisp with an increased productivity without having to learn Emacs.
4. Draw almost all inspiration from the lisp machine (Interlisp and Symbolics) editors of old.

## To Run / To Exit

To run the project, execute the following:
```sh
% make
cc -o cline cline.c -Wall -W -pedantic -std=c99
% ./cline
```

Hit ESC three times to terminate cline.

## Next Steps

1. Finish basic editing
2. Refactor basic editing to a command structure using the "status message" area (Shift-Space).
3. Integrate ECL or SBCL

## To the Author

1. Narrow focus. There should only be text editing (structural formatting) and REPL.
2. Don't reimplement terminal functionality (fonts, multiple buffers, etc) unless necessary.
3. Cline should be more like nano and less like VS Code.