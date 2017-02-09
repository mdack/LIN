/*
*	Milagros del Rocío Peña Quineche 
*/
#include <getopt.h>
#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>

#define MAX_CHARS_MSG 128
typedef enum {
	NORMAL_MSG, /* Mensaje para transferir lineas de la conversacion entre ambos usuarios del chat */
	USERNAME_MSG, /* Tipo de mensaje reservado para enviar el nombre de usuario al otro extremo*/
	END_MSG /* Tipo de mensaje que se envía por el FIFO cuando un extremo finaliza la comunicación */
}message_type_t;

struct chat_message{
	char contenido[MAX_CHARS_MSG]; //Cadena de caracteres (acabada en '\0)
	message_type_t type;
};

static void receiver(const char* path_fifo){
	struct chat_message chat;
	int fd = 0;
	int bytes =sizeof(struct chat_message);
	int fin = 1;
	const int size=sizeof(struct chat_message);
	char mensaje[MAX_CHARS_MSG];
	char name[MAX_CHARS_MSG];

	fd = open(path_fifo, O_RDONLY);

	if(fd < 0){
		perror(path_fifo);
		printf("No se ha podido establecer la conexión\n");
		exit(1);
	}
	else{
		printf("Conexión de recepción establecida!!\n");
		
		while((bytes = read(fd, &chat, size) == size) && fin == 1){
			
			if(chat.type == USERNAME_MSG){
				strcpy(name,chat.contenido);
			}

			if(chat.type == NORMAL_MSG){
					printf("%s dice: ", name);
					strcpy(mensaje,chat.contenido);
					printf("%s\n", mensaje);
			}
			else if(chat.type == END_MSG){
				printf("Conexión finalizada por %s\n", name);
				fin = 0;
			}
		}

		if(bytes < 0){
			printf("Error intentando leer FIFO\n");
		}else if(bytes > 0 && bytes != size){
			printf("Error leyendo el registro\n");
		}
		close(fd);
		exit(0);
	}
}

static void sender(char **argv){
	int fd = 0;
	int bytes = 0;
	struct chat_message chat;
	const int size = sizeof(struct chat_message);
	char mensaje[MAX_CHARS_MSG];
	int i = 0;
	char *name = argv[1];
	const char *path_fifo = argv[2];
	int fin = 1;


	fd = open(path_fifo, O_WRONLY);
  
	if(fd < 0){
		perror(path_fifo);
		printf("No se ha podido establecer la conexión\n");
		exit(1);
	}
	else{
		printf("Conexión de envío establecida!!\n");

		chat.type = USERNAME_MSG;
		strcpy(chat.contenido, name);

		bytes=write(fd, &chat, size);

		if(bytes > 0 && bytes != size){
			fprintf(stderr, "Error enviando usuario\n");
			close(fd);
			exit(1);
		}

		while(fin){

			printf(">");
			fgets(mensaje, 128, stdin);
	
			if(strlen(mensaje) != 0){
				chat.type = NORMAL_MSG;
				strcpy(chat.contenido, mensaje);
			}else{
				chat.type = END_MSG;
				fin = 0;
			}
			bytes=write(fd, &chat, size);
			strcpy(mensaje, "");

			if(bytes > 0 && bytes != size){
				fprintf(stderr, "Error intentando escribir\n");
				close(fd);
				exit(1);
			}
		}
		close(fd);
	}
}

int main(int argc, char **argv){
	pthread_t thd1, thd2;
	

	pthread_create(&thd1,NULL,(void*)sender,(void*)argv);
	pthread_create(&thd2,NULL,(void*)receiver, (void*)argv[3]);


	pthread_join(thd1, NULL);
	pthread_join(thd2, NULL);

	return 0;

}