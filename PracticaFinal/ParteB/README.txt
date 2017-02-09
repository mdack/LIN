----------------------------------------------- Chat - Android x86 -----------------------------------------------------------------------------

VISIÓN GENERAL
------------------------------------------------------------------------------------------------------------------------------------------------
Este programa implementa la emula el comportamiento de un chat sencillo, en lenguaje C.

VISIÓN INTERNA
------------------------------------------------------------------------------------------------------------------------------------------------
Para poder utilizar este sencillo chat es necesario utilizar dos FIFOs(por ejemplo:"/proc/fifo0" y "/proc/fifo1")creados por un módulo 
implementado y montado anteriormente, el cual se denomina fifoprocs, cuya tarea principal es gestionar los FIFOs de tal manera que no
existan problemas de concurrencia a la hora de abrir y cerrar y leer y escribir en los FIFOs.
Al iniciar el programa este se encarga de crear dos procesos simultáneos, uno para leer y otro para escribir en el FIFO, los dos
procesos abren el fichero y si esta operación no da problemas, el proceso que se encarga de escribir, envía el nombre del emisor al FIFO
receptor mendiante un mensaje tipo NAME_USER_MSG, además recibe mensajes de tipo NORMAL_MSG de hasta 128 carácteres,los cuales manda al 
FIFO emisor hasta que recibe "Ctrl+D", que significa que está terminando la conversación y por tanto indicando fin de fichero con un
mensaje tipo END_MSG; mientras tanto el proceso encargado de leer el FIFO, recibe el nombre de usuario y por cada entrada que recibe, 
la muestra por pantalla al receptor (ejemplo: "Fulanito dice: Hola"), esto se produce hasta que uno de los dos, tanto emisor como 
receptor, decide terminar la conversación y, por tanto, este proceso recibe un mensaje de tipo END_MSG, cerrando así el fichero y terminando
la conversación y la finalización de los procesos.

--------------------------------------------------------------------------------------------------------------------------------------------------
La licencia de este programa es General Public License.
