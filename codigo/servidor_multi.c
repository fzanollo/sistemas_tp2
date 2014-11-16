#include <signal.h>
#include <errno.h>

#include "biblioteca.h"

/* Estructura que almacena los datos de una reserva. */
typedef struct {
	int posiciones[ANCHO_AULA][ALTO_AULA];
	int cantidad_de_personas;
	
	int rescatistas_disponibles;
} t_aula;

typedef struct
{
	int cant_personas_pasillo;
	int cant_personas_grupo;

} t_salidas;

typedef struct {
	//Cosas del problema
	int socket_fd;
	t_aula* el_aula;
	t_salidas* salidas;
	//Cosas del pthread
	int tid;
} t_parametros;


/////////////// Definicion de mutexes

pthread_mutex_t mutex_aula;
pthread_mutex_t mutex_posiciones[ANCHO_AULA][ALTO_AULA];
pthread_mutex_t mutex_cantidad_de_personas;
pthread_mutex_t mutex_rescatistas;
pthread_mutex_t mutex_pasillo;
pthread_mutex_t mutex_grupo;

/////////////// Fin definicion de mutexes

////////////// Variables de condicion
pthread_cond_t cond_hay_rescatistas;
pthread_cond_t cond_grupo_lleno;
pthread_cond_t cond_estan_evacuando;
pthread_cond_t cond_salimos_todos;
////////////// Fin variables de condicion

// crear mutex pthread mutex init(mutex, attr)
// destruir mutex pthread mutex destroy(mutex)


void t_aula_iniciar_vacia(t_aula *un_aula)
{
	int i, j;
	for(i = 0; i < ANCHO_AULA; i++)
	{
		for (j = 0; j < ALTO_AULA; j++)
		{
			un_aula->posiciones[i][j] = 0;
		}
	}

	un_aula->cantidad_de_personas = 0;
	
	un_aula->rescatistas_disponibles = RESCATISTAS;
}

void t_aula_ingresar(t_aula *un_aula, t_persona *alumno)
{
	pthread_mutex_lock(&mutex_cantidad_de_personas);
	un_aula->cantidad_de_personas++;
	pthread_mutex_unlock(&mutex_cantidad_de_personas);

	pthread_mutex_lock(&mutex_posiciones[alumno->posicion_fila][alumno->posicion_columna]);
	un_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]++;
	pthread_mutex_unlock(&mutex_posiciones[alumno->posicion_fila][alumno->posicion_columna]);
}

void t_aula_liberar(t_aula *un_aula, t_persona *alumno)
{
	pthread_mutex_lock(&mutex_cantidad_de_personas);
	un_aula->cantidad_de_personas--;
	pthread_mutex_unlock(&mutex_cantidad_de_personas);
}

static void terminar_servidor_de_alumno(int socket_fd, t_aula *aula, t_persona *alumno) {
	printf(">> Se interrumpió la comunicación con una consola.\n");
		
	close(socket_fd);
	
	t_aula_liberar(aula, alumno);
	exit(-1);
}

t_comando intentar_moverse(t_aula *el_aula, t_persona *alumno, t_direccion dir)
{
	int fila = alumno->posicion_fila;
	int columna = alumno->posicion_columna;
	alumno->salio = direccion_moverse_hacia(dir, &fila, &columna);

	///char buf[STRING_MAXIMO];
	///t_direccion_convertir_a_string(dir, buf);
	///printf("%s intenta moverse hacia %s (%d, %d)... ", alumno->nombre, buf, fila, columna);
	
	
	bool entre_limites = (fila >= 0) && (columna >= 0) &&
	     (fila < ALTO_AULA) && (columna < ANCHO_AULA);
	     
	bool pudo_moverse = alumno->salio ||
	    (entre_limites && el_aula->posiciones[fila][columna] < MAXIMO_POR_POSICION);
	
	int anterior_f = alumno->posicion_fila;
	int anterior_c = alumno->posicion_columna;
	int orden_locks;
	
	//Decido en que orden tomar los locks para evitar espera circular
	if(fila < anterior_f || (fila == anterior_f && columna < anterior_c)){
		if(!alumno->salio){
			pthread_mutex_lock(&mutex_posiciones[fila][columna]);
		}
		pthread_mutex_lock(&mutex_posiciones[anterior_f][anterior_c]);
		orden_locks = 0;
	}
	else{
		pthread_mutex_lock(&mutex_posiciones[anterior_f][anterior_c]);
		if(!alumno->salio){
			pthread_mutex_lock(&mutex_posiciones[fila][columna]);
		}
		orden_locks = 1;
	}

	if (pudo_moverse)
	{
		if (!alumno->salio)
			el_aula->posiciones[fila][columna]++;
		el_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]--;
		alumno->posicion_fila = fila;
		alumno->posicion_columna = columna;
	}

	if(orden_locks == 0){
		pthread_mutex_unlock(&mutex_posiciones[anterior_f][anterior_c]);
		if(!alumno->salio){
			pthread_mutex_unlock(&mutex_posiciones[fila][columna]);
		}
	}
	else{
		if(!alumno->salio){
			pthread_mutex_unlock(&mutex_posiciones[fila][columna]);
		}
		pthread_mutex_unlock(&mutex_posiciones[anterior_f][anterior_c]);
	}
	
	//~ if (pudo_moverse)
		//~ printf("OK!\n");
	//~ else
		//~ printf("Ocupado!\n");


	return pudo_moverse;
}

void colocar_mascara(t_aula *el_aula, t_persona *alumno)
{
	printf("Esperando rescatista. Ya casi %s!\n", alumno->nombre);	
	alumno->tiene_mascara = true;
}

void * atendedor_de_alumno(void * params){

	t_parametros * parametros = (t_parametros *) params;
	int socket_fd = parametros->socket_fd;

	t_aula * el_aula = parametros->el_aula;
	t_salidas * salidas = parametros->salidas;


	t_persona alumno;
	t_persona_inicializar(&alumno);

	if (recibir_nombre_y_posicion(socket_fd, &alumno) != 0) {
		/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
		terminar_servidor_de_alumno(socket_fd, NULL, NULL);
	}
	
	printf("Atendiendo a %s en la posicion (%d, %d)\n", 
			alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

	t_aula_ingresar(el_aula, &alumno);

	/// Loop de espera de pedido de movimiento.
	for(;;) {
		t_direccion direccion;
		
		/// Esperamos un pedido de movimiento.
		if (recibir_direccion(socket_fd, &direccion) != 0) {
			/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
			terminar_servidor_de_alumno(socket_fd, el_aula, &alumno);
		}
		printf("Recibi direccion de %s\n", alumno.nombre);
		
		/// Tratamos de movernos en nuestro modelo
		bool pudo_moverse = intentar_moverse(el_aula, &alumno, direccion);

		printf("%s se movio a: (%d, %d)\n", alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

		/// Avisamos que ocurrio
		enviar_respuesta(socket_fd, pudo_moverse ? OK : OCUPADO);		
		//printf("aca3\n");
		
		if (alumno.salio)
			break;
	}
	printf("%s salió\n", alumno.nombre);

	//Salio, hay que ver que haya rescatistas para ponerle la mascara
	pthread_mutex_lock(&mutex_rescatistas);
	printf("Rescatistas disponibles: %d\n", el_aula->rescatistas_disponibles); 
	while(el_aula->rescatistas_disponibles == 0){
		//Debe esperarlos
		pthread_cond_wait(&cond_hay_rescatistas, &mutex_rescatistas);
	}
	//Hay uno disponible, se lo asigno
	printf("%s tiene a un rescatista\n", alumno.nombre); 
	el_aula->rescatistas_disponibles--;
	pthread_mutex_unlock(&mutex_rescatistas);

	colocar_mascara(el_aula, &alumno);

	pthread_mutex_lock(&mutex_rescatistas);
	//El rescatista ya coloco su mascarilla, puede atender a alguien mas
	printf("%s desocupa a un rescatista\n", alumno.nombre);
	el_aula->rescatistas_disponibles++;
	//Avisa que hay rescatistas disponibles.
	pthread_cond_signal(&cond_hay_rescatistas);
	pthread_mutex_unlock(&mutex_rescatistas);

	printf("%s se prepara para salir\n", alumno.nombre);

	pthread_mutex_lock(&mutex_pasillo);
	//Sale efectivamente del aula y se va al pasillo
	t_aula_liberar(el_aula, &alumno);
	salidas->cant_personas_pasillo++;
	printf("%s esta en el pasillo\n", alumno.nombre);
	pthread_mutex_unlock(&mutex_pasillo);

	pthread_mutex_lock(&mutex_grupo);
	//Espera a que haya espacio en el grupo de evacuacion
	while(salidas->cant_personas_grupo >= 5)
		pthread_cond_wait(&cond_grupo_lleno, &mutex_grupo);
	//Hay espacio para uno más
	pthread_mutex_lock(&mutex_pasillo);
	salidas->cant_personas_pasillo--;
	salidas->cant_personas_grupo++;
	pthread_mutex_unlock(&mutex_pasillo);

	//Veo si hay equipo o no hay mas gente en el pasillo 
	if(salidas->cant_personas_grupo == 5 || 
		(salidas->cant_personas_pasillo == 0 && el_aula->cantidad_de_personas == 0)){
		//Salgamos todos!
		pthread_cond_broadcast(&cond_estan_evacuando);
	}
	else{
		//Todavia no podemos salir
		pthread_cond_wait(&cond_estan_evacuando, &mutex_grupo);
	}
	salidas->cant_personas_grupo--;
	//Espero a que termine de salir todo el grupo
	if(salidas->cant_personas_grupo > 0){
		pthread_cond_wait(&cond_salimos_todos, &mutex_grupo);
	}
	else{
		//Ya salimos todos, puede volver a ingresar gente al grupo
		pthread_cond_signal(&cond_salimos_todos);
		pthread_cond_broadcast(&cond_grupo_lleno);
	}

	pthread_mutex_unlock(&mutex_grupo);

	enviar_respuesta(socket_fd, LIBRE);
	
	printf("Listo, %s es libre!\n", alumno.nombre);

	free(params);
	return NULL;

}

int main(void)
{
	//signal(SIGUSR1, signal_terminar);
	int socketfd_cliente, socket_servidor, socket_size;
	struct sockaddr_in local, remoto;

	/* Crear un socket de tipo INET con TCP (SOCK_STREAM). */
	if ((socket_servidor = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("creando socket");
	}

	/* Crear nombre, usamos INADDR_ANY para indicar que cualquiera puede conectarse aquí. */
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(PORT);
	
	if (bind(socket_servidor, (struct sockaddr *)&local, sizeof(local)) == -1) {
		perror("haciendo bind");
	}

	/* Escuchar en el socket y permitir 5 conexiones en espera. */
	if (listen(socket_servidor, 5) == -1) {
		perror("escuchando");
	}
	
	t_aula el_aula;
	t_aula_iniciar_vacia(&el_aula);
	t_salidas salidas;
	salidas.cant_personas_pasillo = 0;
	salidas.cant_personas_grupo = 0;
	
	/// Aceptar conexiones entrantes.
	socket_size = sizeof(remoto);

	// Inicializacion de mutexes
	//pthread_mutex_init(&mutex_aula, NULL);
	pthread_mutex_init(&mutex_rescatistas, NULL);
	pthread_mutex_init(&mutex_pasillo, NULL);
	pthread_mutex_init(&mutex_grupo, NULL);
	pthread_mutex_init(&mutex_cantidad_de_personas, NULL);

	int f, c;
	for(f = 0; f < ANCHO_AULA; f++)
	{
		for (c = 0; c < ALTO_AULA; c++)
		{
			pthread_mutex_init(&mutex_posiciones[f][c], NULL);
		}
	}

	// Variables de condicion
	pthread_cond_init(&cond_hay_rescatistas, NULL);
	pthread_cond_init(&cond_salimos_todos, NULL);
	pthread_cond_init(&cond_grupo_lleno, NULL);
	pthread_cond_init(&cond_estan_evacuando, NULL);
	

	for(;;){		
		if (-1 == (socketfd_cliente = 
					accept(socket_servidor, (struct sockaddr*) &remoto, (socklen_t*) &socket_size)))
		{			
			printf("!! Error al aceptar conexion\n");
		}
		else{
			//cant_clientes++;
			pthread_t tid;
			//Debemos reservar memoria para los parametros de cada pthread
			//Sugerencia: MALLOC (el pthread se debe encargar de liberar memoria)
			t_parametros * parametros = malloc(sizeof(t_parametros));
			
			parametros->socket_fd = socketfd_cliente;
			parametros->el_aula = &el_aula;
			//parametros->tid = cant_clientes;
			parametros->salidas = &salidas; 
			pthread_create(&tid, NULL, atendedor_de_alumno, (void *)parametros);
			//pthread_join(tid, NULL);

			//atendedor_de_alumno(socketfd_cliente, &el_aula);
		}
	}
	return 0;
}


