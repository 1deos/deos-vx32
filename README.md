# vx32 variant #

vx32 is the library that allows intel's 32-bit architecture to be run in a sandbox.
One of the demonstrations is a version of the operating system Plan 9 from Bell Labs that has been modified
to run as a vx32 application, including access to the host file system and network stack (in a similar way to Inferno).
I use it to access Plan 9 systems at home and work from my Linux notebook.

This version of vx32 is close to the original one with a few changes to restrain gcc's code-breaking ability,
some small but important bug fixes, and to add the nsec system call.

Instructions are just the same as for the original version: see the ADVENTURE file at the repository root.