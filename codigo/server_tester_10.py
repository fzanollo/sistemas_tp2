import subprocess as sp
from pokemon import pokemon

for i in xrange(3):
	pos_fila = i*23 % 10
	pos_columna = i*17 % 10
	sp.Popen(["python","pycliente.py", pokemon[i], str(pos_fila), str(pos_columna)])

print "Corri todo"
