//CLIENT
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define STRSTDLEN 30
#define STROGGLEN 128
#define CLIENTLEN 2048
#define SERVER_PORT 12345
#define TIMEOUT 180
#define DIMACK 5
#define DIMREQ 5

int timed_out=0;
char request[5];
char* address;
int logged=0;
struct sockaddr_in server_addr;
char username[STRSTDLEN];
char password[STRSTDLEN];
int ds_sock;
char buf[CLIENTLEN]; //buffer generico usato ad esempio per le recive

void handler(){
	memset(username,0,sizeof(username));
	memset(password,0,sizeof(password));
	timed_out=1;
	logged=0;
	alarm(TIMEOUT);
}

void invia(char* cosa){
	if(send(ds_sock, cosa, strlen(cosa)+1,0)==-1) 
		exit(-1);
}


void ricevi(){
	if(recv(ds_sock,buf,CLIENTLEN,0)==-1)
		exit(-1);
}

void connetti(){
	if((ds_sock=socket(AF_INET,SOCK_STREAM,0))==-1){
		puts("errore con la socket");
		exit(-1);
	}
	struct sockaddr_in server_addr;
	bzero(&server_addr, sizeof(server_addr));
	server_addr.sin_family=AF_INET;
	server_addr.sin_port=htons(SERVER_PORT);
	
	//RICORDA CHE INET_ATON RESTITUISCE GIA' IN NETWORK ORDER
	if((inet_aton(address,&server_addr.sin_addr.s_addr))==0){
		puts("errore irreversibile con inet_aton");
		exit(-1);
	}
	
	if((connect(ds_sock,(struct sockaddr*) &server_addr,sizeof(server_addr)))==-1){
		puts("indirizzo sbagliato oppure server down.. riprovare in seguito");
		exit(-1);
	}
}

void disconnetti(){
	if(close(ds_sock)==-1){
		puts("errore nella close");
		exit(-1);
	}
}

void inviaLogin(){
	connetti();
	invia(request);
	//ack
	ricevi();
	invia(username);
	//ack
	ricevi();
	invia(password);
	//attendi esito
	ricevi();
}

void login(){
	if(logged==1){
		//dico che è un login
		strcpy(request,"si\0");
		inviaLogin();
		//TIMEOUT Secondi a partire dall ultima volta che sono stato attivo
		alarm(TIMEOUT);
		//so giò che è corretto, inoltre so che non era un puro login, quindi non invio 4
		//e non chiudo la comunicazione
	}
	else if(logged==0){
		int login=0;
		int result=0;
		prima:
		//0 sarà una registrazione, 1 un login
		puts("Sei registrato? (si/no)");
		fgets(request,STRSTDLEN,stdin);
		//NON REGISTRATO, AVVIA REGISTRAZIONE
		if(strncmp(request,"no",2)==0){
			puts("registrazione in corso...");
		}
		//REGISTRATO, AVVIA IL LOGIN
		else if(strncmp("si",request,2)==0){
			login=1;
		}
		//NON HA INSERITO NE SI NE NO
		else {goto prima;}	
		while(result!=1){
			puts("inserisci lo username");
			fgets(username, STRSTDLEN, stdin);
			puts("inserisci la password");
			fgets(password,STRSTDLEN,stdin);
			inviaLogin();
			result = atoi(buf);
			if(login==0 && result==0)
				puts("username gia in uso oppure formato non valido, inserisci un altro username");
			else if(login==1 && result==0)
				puts("login fallito, riprovare");
			else{
				puts("logged!!");
				alarm(TIMEOUT);
			}
			//ora il client sa di essere loggato, chiude la connessione
			//in attesa di istruzioni, oppure il login è fallito e la connessione va comunque chiusa
			
			//se invece era scaduto il timeout lui sta provando a fare un altra operazione e quindi non devo
			//inviare la richiesta per una 4
			if(timed_out!=1){
				sprintf(buf,"%d",4);
				invia(buf);
				disconnetti();
			}
		}
		logged=1;
	}
}

void leggiBacheca(){
	login();
	sprintf(request,"%d",1);
	invia(request);
	while(1){
		ricevi();
		if((strcmp("Fine messaggi",buf))==0){
			puts("---------");
			break;
		}
		printf("%s",buf);
		//invio un ack
		strcpy(buf,"\n\0");
		invia(buf);
	}
	//INVIO L'ULTIMO ACK
	strcpy(buf,"\n\0");
	invia(buf);
	disconnetti();
}

void inserire(){
	int ok=0;
	char oggetto[STROGGLEN];
	char messaggio[CLIENTLEN];
	while(ok!=1){
		puts("Inserisci l'oggetto:");
		fgets(oggetto, STROGGLEN, stdin);
		puts("Inserisci il messaggio");
		fgets(messaggio, CLIENTLEN, stdin);
		if(oggetto==NULL || (strcmp(oggetto,""))==0 || (strcmp(oggetto,"\n"))==0){
			puts("Non sono ammessi come oggetto e corpo del messaggio testi vuoti");
			continue;
		}
		if(messaggio==NULL || (strcmp(messaggio,""))==0 || (strcmp(messaggio,"\n"))==0){
			puts("Non sono ammessi come oggetto e corpo del messaggio testi vuoti");
			continue;
		}
		ok=1;
	}
	login();
	sprintf(request,"%d",2);
	invia(request);
	//ack
	ricevi();
	invia(oggetto);
	//ack
	ricevi();
	invia(messaggio);
	//ack
	ricevi();
	disconnetti();
}

void rimuovere(){
	int ok=0;
	char oggetto[STROGGLEN];
	while(ok!=1){
		puts("Inserisci l'oggetto del tuo messaggio che vuoi rimuovere");
		fgets(oggetto,STROGGLEN,stdin);
		if(oggetto==NULL || (strcmp(oggetto,""))==0 || (strcmp(oggetto,"\n"))==0){
			puts("Non sono ammessi come oggetto e corpo del messaggio testi vuoti");
			continue;
		}
		ok=1;
	}
	login();
	sprintf(request,"%d",3);
	invia(request);
	//ack, lui ha capito che vuoi eliminare
	ricevi();
	invia(oggetto);
	//ricevo l'esito
	ricevi();
	disconnetti();
	//stampo l'esito
	printf("%s\n",buf);
}

int main(int argc, char* argv[]){
	
	signal(SIGALRM,handler);
	
	memset(request,0,sizeof(request));
	memset(buf,0,sizeof(buf));
	memset(username,0,sizeof(username));
	memset(password,0,sizeof(password));
	
	if(argc==1){
		puts("usage: ./client <serverIP>");
		exit(-1);
	}
	address = (char*) malloc(sizeof(argv[1]));
	strcpy(address,argv[1]);
	
	login();
	
	while(1){
		puts("Cosa vuoi fare? Scrivi:\n 1.Leggere la bacheca;\n 2.Inserire;\n 3.Rimuovere;\n 4.Uscire.");
		fgets(request, DIMREQ, stdin);
		int in= atoi(request);
		switch(in){
			case 1:
				leggiBacheca();
				break;
			case 2:
				inserire();
				break;
			case 3:
				rimuovere();
				break;
			case 4:
				puts("A presto");
				exit(0);
				break;
			default:
				puts("codice errato, riprova!");
				continue;
		}
	}
	exit(0);
}
