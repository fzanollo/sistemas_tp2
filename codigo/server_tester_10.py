import subprocess as sp
from pokemon import pokemon
import sys

try:
	cant_clientes = sys.argv[1]
	try:
		offset_nombres = sys.argv[2]
	except IndexError:
		offset_nombres = 0
except IndexError:
	cant_clientes = 10
	offset_nombres = 0

for i in xrange(cant_clientes):
	pos_fila = i*23 % 10
	pos_columna = i*17 % 10
	sp.Popen(["python","pycliente.py", pokemon[offset_nombres + i], str(pos_fila), str(pos_columna)])

print "Corri todo"
