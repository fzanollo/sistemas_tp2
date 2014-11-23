#include <signal.h>
#include <errno.h>

#include "biblioteca.h"

/* Estructura que almacena los datos del aula. */
typedef struct {
	int posiciones[ANCHO_AULA][ALTO_AULA];
	int cantidad_de_personas;
	
	int rescatistas_disponibles;
} t_aula;

/* Estructura que almacena los datos de las salidas:pasillo y grupo de evacuación */
typedef struct
{
	int cant_personas_pasillo;
	int cant_personas_grupo;

} t_salidas;

/* Estructura que almacena los parámetros del pthread */
typedef struct {
	//Cosas del problema
	int socket_fd;
	t_aula* el_aula;
	t_salidas* salidas;
	//Cosas del pthread
	//int tid;
} t_parametros;

/* Definición de mutexes */

pthread_mutex_t mutex_posiciones[ANCHO_AULA][ALTO_AULA];
pthread_mutex_t mutex_cantidad_de_personas;
pthread_mutex_t mutex_rescatistas;
pthread_mutex_t mutex_pasillo;
pthread_mutex_t mutex_grupo;

/* Definición de variables de condición*/

pthread_cond_t cond_hay_rescatistas;
pthread_cond_t cond_grupo_lleno;
pthread_cond_t cond_estan_evacuando;
pthread_cond_t cond_salimos_todos;

// Inicializar el aula
void t_aula_iniciar_vacia(t_aula *un_aula)
{
	// Inicializar cada posición del aula
	int i, j;
	for(i = 0; i < ANCHO_AULA; i++)
	{
		for (j = 0; j < ALTO_AULA; j++)
		{
			un_aula->posiciones[i][j] = 0;
		}
	}

	// Inicializar la cantidad de personas
	un_aula->cantidad_de_personas = 0;
	
	// Inicializar la cantidad de rescatistas disponibles
	un_aula->rescatistas_disponibles = RESCATISTAS;
}

// Registrar a un nuevo alumno en el aula
void t_aula_ingresar(t_aula *un_aula, t_persona *alumno)
{
	// Aumentar la cantidad de personas del aula
	pthread_mutex_lock(&mutex_cantidad_de_personas);
		un_aula->cantidad_de_personas++;
	pthread_mutex_unlock(&mutex_cantidad_de_personas);

	// Registrar posición del alumno
	pthread_mutex_lock(&mutex_posiciones[alumno->posicion_fila][alumno->posicion_columna]);
		un_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]++;
	pthread_mutex_unlock(&mutex_posiciones[alumno->posicion_fila][alumno->posicion_columna]);
}

// Remover a un alumno del aula
void t_aula_liberar(t_aula *un_aula, t_persona *alumno)
{
	// Disminuir la cantidad de personas del aula
	pthread_mutex_lock(&mutex_cantidad_de_personas);
		un_aula->cantidad_de_personas--;
	pthread_mutex_unlock(&mutex_cantidad_de_personas);
}

// Cerrar la conexión con el cliente
static void terminar_servidor_de_alumno(int socket_fd, t_aula *aula, t_persona *alumno, void * params) {
	printf(">> Se interrumpió la comunicación con una consola.\n");
	
	// Cerrar el socket
	close(socket_fd);
	
	// Remover al alumno
	t_aula_liberar(aula, alumno);
	// Libero memoria de los parámetros
	free(params);
	// Cierro el pthread
	pthread_exit(0);
	//exit(-1);
}

// Mover al alumno dentro del aula, si es posible. Sino, reportar que no pudo moverse
t_comando intentar_moverse(t_aula *el_aula, t_persona *alumno, t_direccion dir)
{
	int fila = alumno->posicion_fila;
	int columna = alumno->posicion_columna;
	// Calcular nueva posición
	alumno->salio = direccion_moverse_hacia(dir, &fila, &columna);

	// Si la nueva posición está dentro del aula 
	bool entre_limites = (fila >= 0) && (columna >= 0) &&
	     (fila < ALTO_AULA) && (columna < ANCHO_AULA);
	   
	// Si la nueva posición es válida, o es la salida.
	pthread_mutex_lock(&mutex_posiciones[fila][columna]);
		bool pudo_moverse = alumno->salio ||
		    (entre_limites && el_aula->posiciones[fila][columna] < MAXIMO_POR_POSICION);
	pthread_mutex_unlock(&mutex_posiciones[fila][columna]);
	
	int anterior_f = alumno->posicion_fila;
	int anterior_c = alumno->posicion_columna;
	int orden_locks;

	if (pudo_moverse){
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

		// Actualizar posición
		if (!alumno->salio)
			el_aula->posiciones[fila][columna]++;
		el_aula->posiciones[alumno->posicion_fila][alumno->posicion_columna]--;
		alumno->posicion_fila = fila;
		alumno->posicion_columna = columna;

		// Unlocks en orden también
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
	} 
	
	return pudo_moverse;
}

// Colocar máscara al alumno 
void colocar_mascara(t_aula *el_aula, t_persona *alumno)
{
	printf("Esperando rescatista. Ya casi %s!\n", alumno->nombre);	
	alumno->tiene_mascara = true;
}

// Interactuar con el cliente del alumno
void * atendedor_de_alumno(void * params){

	t_parametros * parametros = (t_parametros *) params;
	int socket_fd = parametros->socket_fd;
	t_aula * el_aula = parametros->el_aula;
	t_salidas * salidas = parametros->salidas;

	t_persona alumno;
	// Inicializar la estructura del alumno
	t_persona_inicializar(&alumno);

	if (recibir_nombre_y_posicion(socket_fd, &alumno) != 0) {
		/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
		terminar_servidor_de_alumno(socket_fd, NULL, NULL, NULL);
	}
	
	printf("Atendiendo a %s en la posicion (%d, %d)\n", 
			alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

	// Registrar alumno en el aula
	t_aula_ingresar(el_aula, &alumno);

	/// Loop de espera de pedido de movimiento.
	for(;;) {
		t_direccion direccion;
		
		/// Esperar un pedido de movimiento.
		if (recibir_direccion(socket_fd, &direccion) != 0) {
			/* O la consola cortó la comunicación, o hubo un error. Cerramos todo. */
			terminar_servidor_de_alumno(socket_fd, el_aula, &alumno, params);
		}
		printf("Recibi direccion de %s\n", alumno.nombre);
		
		/// Calcular nueva posición o informar que no se puede mover a esa dirección
		bool pudo_moverse = intentar_moverse(el_aula, &alumno, direccion);

		printf("%s se movio a: (%d, %d)\n", alumno.nombre, alumno.posicion_fila, alumno.posicion_columna);

		/// Responderle al cliente el resultado de su acción
		enviar_respuesta(socket_fd, pudo_moverse ? OK : OCUPADO);		
		
		// Si la posición actual del alumno es la salida, proceder hacia el pasillo
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

	// Colocar máscara con el rescatista asignado
	colocar_mascara(el_aula, &alumno);

	pthread_mutex_lock(&mutex_rescatistas);
		//El rescatista ya coloco su mascarilla, puede atender a alguien mas
		printf("%s desocupa a un rescatista\n", alumno.nombre);
		el_aula->rescatistas_disponibles++;
	pthread_mutex_unlock(&mutex_rescatistas);
	//Avisa que hay al menos un rescatista disponible
	pthread_cond_signal(&cond_hay_rescatistas);

	printf("%s se prepara para salir\n", alumno.nombre);

	pthread_mutex_lock(&mutex_pasillo);
		//Sale efectivamente del aula y se va al pasillo
		t_aula_liberar(el_aula, &alumno);
		salidas->cant_personas_pasillo++;
		printf("%s esta en el pasillo\n", alumno.nombre);
	pthread_mutex_unlock(&mutex_pasillo);

	pthread_mutex_lock(&mutex_grupo);
		//Espera a que haya espacio en el grupo de evacuacion
		while(salidas->cant_personas_grupo >= 5){
			printf("%s está esperando a que evacúe el grupo\n", alumno.nombre);	
			pthread_cond_wait(&cond_grupo_lleno, &mutex_grupo);
		}
		//Hay espacio para uno más
		pthread_mutex_lock(&mutex_pasillo);
			salidas->cant_personas_pasillo--;
			printf("%s sale del pasillo donde quedan %d personas\n", alumno.nombre, salidas->cant_personas_pasillo);
			printf("%s se une al grupo de %d personas a ser evacuadas\n", alumno.nombre, salidas->cant_personas_grupo);
			salidas->cant_personas_grupo++;
		pthread_mutex_unlock(&mutex_pasillo);

		//Veo si hay equipo o no hay mas gente en el pasillo
		pthread_mutex_lock(&mutex_pasillo);
		pthread_mutex_lock(&mutex_cantidad_de_personas); 
		if(salidas->cant_personas_grupo == 5 || 
			(salidas->cant_personas_pasillo == 0 && el_aula->cantidad_de_personas == 0)){
				pthread_mutex_unlock(&mutex_cantidad_de_personas);
				pthread_mutex_unlock(&mutex_pasillo);
				// Hay equipo, avisar al resto del grupo que ya pueden empezar a evacuar
				printf("%s avisa al grupo de %d personas que ya pueden a ser evacuados\n", alumno.nombre, salidas->cant_personas_grupo);
				pthread_cond_broadcast(&cond_estan_evacuando);
		}
		else{
			pthread_mutex_unlock(&mutex_cantidad_de_personas);
			pthread_mutex_unlock(&mutex_pasillo);
			//Todavia no podemos salir, falta gente
			printf("%s espera junto al grupo de %d personas\n", alumno.nombre, salidas->cant_personas_grupo);
			pthread_cond_wait(&cond_estan_evacuando, &mutex_grupo);
		}
		// Evacuar efectivamente
		salidas->cant_personas_grupo--;
		printf("%s fue evacuado\n", alumno.nombre);
		//Espero a que termine de salir todo el grupo
		if(salidas->cant_personas_grupo > 0){
			printf("Aún quedan %d personas por ser evacuadas\n", salidas->cant_personas_grupo);
			pthread_cond_wait(&cond_salimos_todos, &mutex_grupo);
		}
		else{
			//Ya salimos todos, puede volver a ingresar gente al grupo
			printf("Ya salimos todos, pueden volver a evacuar gente\n");
			pthread_cond_broadcast(&cond_salimos_todos);
			pthread_cond_broadcast(&cond_grupo_lleno);
		}

	pthread_mutex_unlock(&mutex_grupo);

	// Respuesta final al cliente
	enviar_respuesta(socket_fd, LIBRE);
	
	printf("Listo, %s es libre!\n", alumno.nombre);

	// Liberar memoria
	free(params);
	return NULL;

}

int main(void)
{
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
	pthread_mutex_init(&mutex_rescatistas, NULL);
	pthread_mutex_init(&mutex_pasillo, NULL);
	pthread_mutex_init(&mutex_grupo, NULL);
	pthread_mutex_init(&mutex_cantidad_de_personas, NULL);

	// Un mutex por posicion del aula
	int f, c;
	for(f = 0; f < ANCHO_AULA; f++)
	{
		for (c = 0; c < ALTO_AULA; c++)
		{
			pthread_mutex_init(&mutex_posiciones[f][c], NULL);
		}
	}

	// Inicialización de variables de condición
	pthread_cond_init(&cond_hay_rescatistas, NULL);
	pthread_cond_init(&cond_salimos_todos, NULL);
	pthread_cond_init(&cond_grupo_lleno, NULL);
	pthread_cond_init(&cond_estan_evacuando, NULL);
	
	// Loop de atención del clientes
	for(;;){		
		if (-1 == (socketfd_cliente = 
					accept(socket_servidor, (struct sockaddr*) &remoto, (socklen_t*) &socket_size)))
		{	
			// Hubo un error de conexión con el cliente
			printf("!! Error al aceptar conexion\n");
		}
		else{
			// Se conectó un nuevo cliente
			// Inicializar un pthread para interactuar con el cliente
			printf("Hay un nuevo cliente\n");
			pthread_t tid;
			// Reservar memoria dinámica para cada pthread
			// La función del pthread se encargará de liberar la memoria
			t_parametros * parametros = malloc(sizeof(t_parametros));
			
			// Inicialización de los parámetros del pthread
			parametros->socket_fd = socketfd_cliente;
			parametros->el_aula = &el_aula;
			parametros->salidas = &salidas; 

			// Crear pthread con la funcion 'atendedor_de_alumno'
			pthread_create(&tid, NULL, atendedor_de_alumno, (void *)parametros);
			printf("Creamos un thread para el nuevo cliente\n");
		}
	} 
	return 0;
}