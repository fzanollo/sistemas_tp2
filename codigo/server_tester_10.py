import subprocess as sp
from paises import paises

for i in xrange(10):
	pos_fila = i*23 % 10
	pos_columna = i*17 % 10
	sp.Popen(["python","pycliente.py", paises[i], str(pos_fila), str(pos_columna)])

print "Corri todo"
