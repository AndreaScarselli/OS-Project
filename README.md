This program has been realized as a project done for the Operating System course (Bachelor degree). I attempted this course in the 2015
at University of Rome "La Sapienza".

Realization of an online showcase resident on a server. 
An electronic showcase is a service that allows at each authorized user (which is on any machine) to send messages that 
can be read by any other user interested to consult the showcase. In this case the showcase is constituted by a server 
program that sequentially accepts and process the requests of one or more client processes (the client could be, in general, 
resident on different machine). The client program has to offer the following functions:
1)	Read all the messages on the showcase;
2)	Send a new message to the showcase;
3)	Remove one message from the showcase (if the message was sent by the same user who wants to delete it, this verify has to be done by a student-chosen mechanism of authentication).
A message has to contains at least the fields: user, object and text.
