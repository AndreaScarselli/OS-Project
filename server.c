//server

#include <stdio.h>
#include <sys/shm.h>
#include <stdlib.h>  
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h> //for map_fixed....
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

#define BACKLOG 128
#define STRSTDLEN 30
#define STROGGLEN 128
#define STRMESLEN 2048
#define ALARM 180
#define PAGE_SIZE 4096
#define SERVER_PORT 12345
#define DIMACK 5

struct nodoLogin{
	char username[STRSTDLEN];
	char password[STRSTDLEN];
	struct nodoLogin *successivo;
};

struct nodoMessaggi{
	char mittente[STRSTDLEN];
	char oggetto[STROGGLEN];
	char messaggio[STRMESLEN];
	struct nodoMessaggi *successivo;
};

//suppress warning
void salvataggio();

//VARIABILI GLOBALI
int status;
int ds_listen;
int ds_comunication;
int ds_msg;
int ds_pwd;
int ds_usr;
int dirtymsg=0; //bool
int dirtyusr=0; //bool
int msg_eliminati=0; // durante l'esecuzione è stato eliminato qualche messaggio? bool
char toSend[STRMESLEN];
char ack[DIMACK];
pthread_t tid;
sigset_t fillset;
sigset_t segvSet;
sigset_t pipeSet;
struct nodoLogin *testaLogin;
struct nodoLogin *ultimoLoginCaricato;
struct nodoLogin *codaLogin;
struct nodoMessaggi *testaMessaggi;
struct nodoMessaggi *ultimoMessaggioCaricato;
struct nodoMessaggi *codaMessaggi;
struct sigaction general_act;
struct sigaction sigsegv_act;
struct sigaction sigpipe_act;
struct sockaddr_in my_addr;

void arma(int signal, struct sigaction act){
	if((sigaction(signal,&act,NULL))==-1){
		puts("errore mentre armavo il segnale");
		raise(SIGILL);
	}
}

void fineLavoroDelThread(){
	//RIARMO SIGALRM
	arma(SIGALRM,general_act);
	//QUESTO SI FA PER ASSICURARMI CHE, SE L'ALLARME ERA SCATTATA MENTRE IL SEGNALE ERA IGNORATO, LO RIALZIAMO SUBITO
	//VISTO CHE ALARM MI RITORNA IL TEMPO RIMANENTE ALL'ALLARME.. POTREBBE SUCCEDERE CHE L'ALLARME NON ERA ANCORA SCATTATO,
	//SE VENIAMO DESCHEDULATI TRA ALARM(ALARM) E ALARM(TEMPO_RIMANENTE)  PERDIAMO UN PO DI TEMPO MA PAZIENZA...  
	int tempo_rimanente = alarm(ALARM);
	if(tempo_rimanente==0)
		raise(SIGALRM);
	else
		alarm(tempo_rimanente);
	//RICORDA CHE IL THREAD E' "GESTIONESESSIONE"
	status=0;
	pthread_exit((void*) &status);
}

void esci(){
	close(ds_comunication);
	close(ds_msg);
	close(ds_pwd);
	close(ds_usr);
	close(ds_listen);
	exit(0);
}

void sig_exit() {
	printf("\nsalvataggio prima di uscire..\n");
	salvataggio();
	puts("salvataggio completato, esco.");
	esci();
}


void ricevi(char* buf, int dimensione){
	//mi comport esattamente come sotto con le stesse considerazioni
	//questa funzione è void ma fa side effect
	if(recv(ds_comunication,buf,dimensione,0)==-1)
		raise(SIGILL);
}

void invia(int withack){
	//l'unica segnalazione che può arrivare quando sto facendo una send
	//è una segnalazione che porta a terminazione, quindi non la gestisco, tanto il processo terminerà.
	//se la send fallisce per qualsiasi altro motivo è un errore inatteso e quindi non so gestirlo e termino.
	if(send(ds_comunication, toSend, strlen(toSend)+1,0)==-1) 
		raise(SIGILL);
	if(withack==1)
		ricevi(ack,DIMACK);
}

void gestione_sigsegv(int n, siginfo_t *info, void* not){
	long address = (long) (info->si_addr);
	printf("segfault occurred (address is %ld)\nprovo a risolvere \n",address);
	//MAP SHARED PER GLI ALTRI PROCESSI
	//MAP ANON SIGNIFICA CHE IL MAPPING NON RIGUARDA ALCUN FILE, FA DIVENTARE FileDescriptor (4) E OFFSET (5) DON'T CARE
	//MAP FIXED SIGNIFICA: KERNEL, FALLO A QUELL'INDIRIZZO, SE NON CI RIESCI, FALLISCI!
	
	if((mmap(info->si_addr, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON | MAP_FIXED ,0,0))== MAP_FAILED){
		puts("non riesco a validare la pagina del sigsegv...Creo log.txt e termino");
		int ds_log;
		if((ds_log= open("log.txt", O_WRONLY | O_CREAT | O_APPEND, 0666))==-1){
				puts("errore nell'apertura del file di log");
				raise(SIGILL);
		}
		
		//uso tosend per la conversione del long in stringa
		sprintf(toSend, "%ld\n", address);
		write(ds_log, toSend, strlen(toSend));
		close(ds_log);
		//termino
		raise(SIGILL);
	}
	else puts("pagina validata");
}

void pipe_handler(){
	puts("host disconnesso... broken pipe!");
	if(close(ds_comunication)==-1){
		puts("errore nella close(ds_comunication)");
		raise (SIGILL);
	}
	fineLavoroDelThread();
	
}

void generic_handler(int sig){
	switch(sig){
		case SIGHUP: case SIGINT: case SIGQUIT:  case SIGILL: case SIGTERM:
			sig_exit();
			break;
		case SIGALRM:
			if(dirtymsg!=0 || dirtyusr!=0)
				salvataggio();
			else
				puts("nulla da salvare");
			//RISETTA L'ALLARME
			alarm(ALARM);
			break;
		default:
			break;
	}
}

void inserimentoLogin(struct nodoLogin nuovo){
	if(testaLogin==NULL){
		testaLogin=sbrk(sizeof(nuovo));
		memcpy(testaLogin,&nuovo,sizeof(nuovo));
		codaLogin=testaLogin;
	}
	else{
		codaLogin->successivo = sbrk(sizeof(nuovo));
		memcpy(codaLogin->successivo,&nuovo,sizeof(nuovo));
		codaLogin = codaLogin->successivo;
	}
}

//RITORNA 1 SE LA REGISTRAZIONE VA A BUON FINE, 0 ALTRIMENTI
int registrazione(char user[STRSTDLEN], char pwd[STRSTDLEN]){
	//SE LA PASSWORD NON È VALIDA 
	if(pwd==NULL || (strcmp("",pwd))==0 || (strcmp(pwd, "\n"))==0){
			return 0;
	}
	//LOGIN CON PASS==NULL RESTITUISCE 2 SE L'USERNAME ESISTE GIÀ, SE QUESTO ACCADE NON HA SENSO CONTINUARE
	if(user!=NULL && (strcmp("",user))!=0 && (strcmp(user, "\n"))!=0 && (login(user,NULL))!=2){
		//SE IL NOME UTENTE NON È GIA IN USO (E NON È DEL FORMATO SCORRETTO)...
		struct nodoLogin nuovo;
		memcpy(nuovo.username,user,STRSTDLEN);
		memcpy(nuovo.password,pwd,STRSTDLEN);
		nuovo.successivo=NULL;
		inserimentoLogin(nuovo);
		dirtyusr=1;
		return 1;
	}
	else
		return 0;
}


//0 se il login è scorretto
//1 se il login è giusto
//2 se l'username esiste, riutilizzo del codice per altra funzione
//PASS = NULL SIGNIFICA CHE STO FACENDO UNA QUERY SULL'USR
int login(char user[STRSTDLEN], char pass[STRSTDLEN]){
	//non è registrato ancora nessuno o siamo all inizio del restore
	if(testaLogin==NULL) return 0;
	struct nodoLogin *nodeRunner=testaLogin;
	//SE PASSWORD E USER SONO DI QUESTO TIPO INUTILE CHE SPRECO CPU (se = null proseguo altrimenti strcmp segfault)
	if(pass!=NULL && ((strcmp("",pass)==0) || (strcmp(pass, "\n"))==0))
			return 0;
	if(user==NULL || (strcmp("",user))==0 || (strcmp(user, "\n"))==0)
			return 0;
	while(nodeRunner!=NULL){
		if(strcmp(nodeRunner->username,user)==0){
			//USERNAME ESISTE, ERA SOLO UNA QUERY
			if(pass==NULL)
				return 2;
			//USERNAME ESISTE, PASSWORD GIUSTA
			if(strcmp(nodeRunner->password,pass)==0)
				return 1;
			//USERNAME ESISTE, PASSWORD ERRATA
			else
				return 0;
		}
		nodeRunner= nodeRunner->successivo;
	}
	//UTENTE NON TROVATO
	return 0;
}


//"" ARRIVA SE BROKEN PIPE
void inserimentoMessaggio(struct nodoMessaggi nuovo){
	//LO USER NON PUÒ ESSERE NULL PERCHÈ DEVE ESSERE LOGGATO PER CHIAMARE INSERISCI
	if(nuovo.oggetto==NULL || (strcmp(nuovo.oggetto,""))==0 || (strcmp(nuovo.oggetto,"\n"))==0){
			return;
	}
	if(nuovo.messaggio==NULL || (strcmp(nuovo.messaggio,""))==0 || (strcmp(nuovo.messaggio,"\n"))==0){
			return;
	}
	if(testaMessaggi==NULL){
		testaMessaggi=sbrk(sizeof(nuovo));
		memcpy(testaMessaggi,&nuovo,sizeof(nuovo));
		codaMessaggi=testaMessaggi;
		dirtymsg=1;
	}
	else{
		codaMessaggi->successivo=sbrk(sizeof(nuovo));
		memcpy(codaMessaggi->successivo,&nuovo,sizeof(nuovo));
		codaMessaggi=codaMessaggi->successivo;
		dirtymsg=1;
	}
}

//L'USERNAME LOGGATO CHIAMA QUESTA FUNZIONE, QUI NON CONTROLLO COSA MI STA ARRIVANDO, LO FACCIO NELLA FUNZIONE CHIAMATA IN CODA
void inserisciMessaggio(char username[STRSTDLEN]){
	struct nodoMessaggi nuovo;
	strcpy(nuovo.mittente,username);
	//vengono mandati ack per sincronizzazione col client
	strcpy(toSend,"\n\0");
	invia(0);
	ricevi(nuovo.oggetto, STROGGLEN);
	invia(0);
	ricevi(nuovo.messaggio, STRMESLEN);
	invia(0);
	nuovo.successivo=NULL;
	inserimentoMessaggio(nuovo);
}

void eliminaMessaggio(char username[STRSTDLEN]){
	char oggetto[STROGGLEN];
	int bool=0;
	struct nodoMessaggi *nodeRunner= testaMessaggi;
		
	//un ack per dire che sono qua
	strcpy(toSend,"\n\0");
	invia(0);
	//oggetto da rimuovere
	ricevi(oggetto,STROGGLEN);
	//SO GIÀ CHE NON SONO OGGETTI AMMESSI... INUTILE CHE SPRECO CPU
	if(oggetto==NULL || (strcmp(oggetto,""))==0 || (strcmp(oggetto,"\n"))==0){
		bool=0;
	}
	else{
		//SE QUELLO CHE VOGLIO ELIMINARE È PROPRIO LA TESTA È FACILE
		if(testaMessaggi!=NULL && strcmp(testaMessaggi->mittente,username)==0 && strcmp(testaMessaggi->oggetto,oggetto)==0){
			//se il primo è anche l'ultimo, aggiorna la codaMessaggi
			if(codaMessaggi==testaMessaggi)
				codaMessaggi=NULL;
			//in ogni caso:
			testaMessaggi= testaMessaggi->successivo;
			msg_eliminati=1;
			bool=1;
		}
		else{
			while(nodeRunner!=NULL){
				if(nodeRunner->successivo!=NULL && strcmp(nodeRunner->successivo->mittente,username)==0){
					if(strcmp(nodeRunner->successivo->oggetto,oggetto)==0){
						nodeRunner->successivo = (nodeRunner->successivo)->successivo;
						if(nodeRunner->successivo==NULL)
							codaMessaggi=nodeRunner;
						msg_eliminati=1;
						bool=1;
						break;
					}
				}
				nodeRunner = nodeRunner->successivo;
			}
		}
	}
	if(!bool){
		strcpy(toSend,"Messaggio non trovato");
		invia(0);
	}else{
		dirtymsg=1;
		strcpy(toSend,"Messaggio eliminato");
		invia(0);
	}
}

void visualizzaMessaggi(){
	struct nodoMessaggi *nodeRunner= testaMessaggi;
	while(nodeRunner!=NULL){
		strcpy(toSend,"Messaggio inviato da:");
		invia(1);
		strcpy(toSend, nodeRunner->mittente);
		invia(1);
		//
		strcpy(toSend,"Oggetto:");
		invia(1);
		strcpy(toSend, nodeRunner->oggetto);
		invia(1);
		//
		strcpy(toSend,"Messaggio:");
		invia(1);
		strcpy(toSend, nodeRunner->messaggio);
		invia(1);
		//
		strcpy(toSend,"--\n");
		invia(1);
		//
		nodeRunner = nodeRunner->successivo;
	}
	strcpy(toSend,"Fine messaggi");
	invia(1);
}


void autenticazione(char* rcv, char* rcv2){
	int LOGIN=0;
	char cosa[STRSTDLEN];
	ricevi(cosa,STRSTDLEN);
	strcpy(toSend,"\n\0");
	//ack
	invia(0);
	if(strncmp(cosa,"si",2)==0)
		//è un login
		LOGIN=1;
	//altrimenti è una registrazione
	ricevi(rcv, STRSTDLEN); //usr
	//ack
	invia(0);
	ricevi(rcv2, STRSTDLEN); //pwd
	if(LOGIN==0){
		if(registrazione(rcv,rcv2)!=0){
			//registrazione ok
			strcpy(toSend, "1\0");
			invia(0);
		}
		else{
			strcpy(toSend,"0\0");
			invia(0);
		}
	}
	else if(LOGIN==1){
		if(login(rcv,rcv2)!=0){
			//login corretto
			strcpy(toSend,"1\0");
			invia(0);
		}
		else{
			strcpy(toSend,"0\0");
			invia(0);
		}
	}
}


void * gestisciSessione(void* param){
	//NON VOGLIO ESSERE INTERROTTO
	signal(SIGALRM,SIG_IGN);
	char rcv[STRSTDLEN];
	char rcv2[STRSTDLEN];
	autenticazione(rcv, rcv2);
	//risposta a cosa vuoi fare
	ricevi(rcv2,STRSTDLEN);
	int choose= atoi(rcv2);
	switch(choose){
		case 1: visualizzaMessaggi(); goto esci; break;
		case 2: inserisciMessaggio(rcv); goto esci; break;
		case 3: eliminaMessaggio(rcv); goto esci; break;
		case 4: goto esci; break;
		default: break; //hai inserito male
	}
	esci:
	if(close(ds_comunication)==-1){
		puts("errore nella close(ds_comunication)");
		raise (SIGILL);
	}
	
	//sessione finita
	fineLavoroDelThread();
}


void scriviMessaggi(struct nodoMessaggi* msgRunner){
	while(msgRunner!=NULL){
		write(ds_msg, msgRunner->mittente, strlen(msgRunner->mittente));
		write(ds_msg, msgRunner->oggetto, strlen(msgRunner->oggetto));
		write(ds_msg, msgRunner->messaggio, strlen(msgRunner->messaggio));
		msgRunner = msgRunner->successivo;
	}
}

void salvataggio(){
	puts("saving...");
	//backuppo IL LOGIN;
	if(dirtyusr==1){
		//rimetto i file pointer alla fine del file
		lseek(ds_usr,0,2);
		lseek(ds_pwd,0,2);
		
		struct nodoLogin *loginRunner;
		if(ultimoLoginCaricato!=NULL)
			//se questo successivo non esiste dirtyusr sarà a 0.
			loginRunner = (ultimoLoginCaricato->successivo);	
		
		//se succede che è null è perchè è il primo giro...
		else
			loginRunner=testaLogin;
			
			
		while(loginRunner!=NULL){	
			write(ds_usr, loginRunner->username, strlen(loginRunner->username));
			write(ds_pwd, loginRunner->password, strlen(loginRunner->password));
			loginRunner= loginRunner->successivo;
		}
		
		//devo spostarlo all ultimo che è salvato
		ultimoLoginCaricato=codaLogin;
		dirtyusr=0;
	}
	//BACKUPPO I MESSAGGI
	if(dirtymsg==1){
		struct nodoMessaggi *msgRunner;
		if(msg_eliminati==1){
			close(ds_msg);
			//DEVO TRONCARLO PERCHE' I MESSAGGI SONO STATI ELIMINATI
			if((ds_msg = open("messaggi.txt", O_RDWR | O_CREAT | O_TRUNC, 0600))==-1){
				puts("errore nell'apertura del file messaggi");
				raise(SIGILL);
			}
		}
		//se sono stati eliminati messaggi, o è il primo salvataggio:
		if(msg_eliminati==1 || ultimoMessaggioCaricato==NULL){
			msgRunner = testaMessaggi;
			scriviMessaggi(msgRunner);
			msg_eliminati=0;
		}
		else{
			//porto il file pointer alla fine del file
			lseek(ds_msg,0,2);
			msgRunner=ultimoMessaggioCaricato->successivo;
			scriviMessaggi(msgRunner);
		}
		//sposto l'indice
		ultimoMessaggioCaricato=codaMessaggi;
		dirtymsg=0;
	}
	puts("end saving");
}

void restore(){
	
	struct nodoMessaggi nuovo;
	struct nodoLogin nuovoLG;
	
	//per la fgets mi serve un "FILE", non basta un File descriptor
	FILE *fpm = fopen("messaggi.txt", "r");
	char mittente[STRSTDLEN];
	char oggetto[STROGGLEN];
	char messaggio[STRMESLEN];

	
	while((fgets(mittente,STRSTDLEN,fpm))!=NULL){
		fgets(oggetto,STROGGLEN,fpm);
		fgets(messaggio,STRMESLEN,fpm);
		strcpy(nuovo.mittente,mittente);
		strcpy(nuovo.oggetto,oggetto);
		strcpy(nuovo.messaggio,messaggio);
		nuovo.successivo=NULL;
		inserimentoMessaggio(nuovo);
	}
	char username[STRSTDLEN];
	char password[STRSTDLEN];
	
	FILE *fpu = fopen("username.txt", "r");
	FILE *fpp = fopen("password.txt", "r");
	
 	while((fgets(username,STRSTDLEN,fpu))!=NULL){
 		fgets(password,STRSTDLEN,fpp);
 		registrazione(username,password);
 	}
 	
 	fclose(fpu);
 	fclose(fpp);
 	fclose(fpm);
 	
 	//setto quali sono gli ultimi caricati
 	ultimoLoginCaricato=codaLogin;
 	ultimoMessaggioCaricato=codaMessaggi;
 	
 	//sfruttando registrazione e inserimento messaggio mi trovo i dirty a 1
 	dirtymsg=0;
 	dirtyusr=0;
 	
}

void sistemaSegnali(){

	signal(SIGCHLD,SIG_IGN);
	signal(SIGUSR1,SIG_IGN);
	signal(SIGUSR2,SIG_IGN);
	
	//PER I SEGNALI CHE MI FARANNO SALVARE LI BLOCCHERÒ TUTTI
	sigfillset(&fillset);
	general_act.sa_mask=fillset;
	general_act.sa_handler=generic_handler;
	general_act.sa_restorer=NULL;
	general_act.sa_flags=0;
	
	//PIPEACT AMMETTE TUTTI I SEGNALI
	//SIGALRM ALL INIZIO È MASCHERATO DALLA SESSIONE CHE ERA IN CORSO
	sigemptyset(&pipeSet);
	sigpipe_act.sa_mask=pipeSet;
	sigpipe_act.sa_handler=pipe_handler;
	sigpipe_act.sa_restorer=NULL;
	sigpipe_act.sa_flags=0;
	
	//CON SIGSEGV VOGLIO ESSERE INTERROTTO PER CHIUSURA MA NON PER ALLARMI
	sigemptyset(&segvSet);
	sigaddset(&segvSet, SIGALRM);
	sigsegv_act.sa_mask=segvSet;
	sigsegv_act.sa_flags = SA_SIGINFO;
	sigsegv_act.sa_restorer= NULL;
	sigsegv_act.sa_sigaction= gestione_sigsegv;
	
	//SIGHUP,SIGNINT,SIGQUIT,SIGILL,SIGTERM PROVOCANO IL SALVATAGGIO E L'USCITA
	//SIGALARM PROVOCA IL SALVATAGGIO
	arma(SIGHUP,general_act);
	arma(SIGINT,general_act);
	arma(SIGILL,general_act);
	arma(SIGQUIT,general_act);
	arma(SIGTERM,general_act);
	arma(SIGALRM,general_act);
	arma(SIGPIPE,sigpipe_act);
	arma(SIGSEGV,sigsegv_act);
}

void aperturaFile(){
	//CREAZIONE DEI FILE (O APERTURA SE GIÀ ESISTENTI)
	if((ds_msg = open("messaggi.txt", O_RDWR | O_CREAT, 0600))==-1){
		puts("errore nell'apertura del file messaggi");
		raise(SIGILL);
	}
	
	if((ds_usr = open("username.txt", O_RDWR | O_CREAT, 0600))==-1){
		puts("errore nell'apertura del file username");
		raise(SIGILL);
	}
	
	if((ds_pwd = open("password.txt", O_RDWR | O_CREAT, 0600))==-1){
		puts("errore nell'apertura del file password");
		raise(SIGILL);
	}
}

void binding(){
	
	if((ds_listen=socket(AF_INET,SOCK_STREAM,0))==-1){
		puts("errore nella chiamata a socket");
		raise(SIGILL);
	}
	
		
	//rendo l'indirizzo subito riutilizzabile
	int param = 1;
	//param è true se non è zero
	if(setsockopt(ds_listen, SOL_SOCKET, SO_REUSEADDR, &param, sizeof(param))==-1){
		puts("errore nella setsockopt");
		raise(SIGILL);
	}
	
	bzero((char*)&my_addr, sizeof(my_addr));
	my_addr.sin_family=AF_INET;
	my_addr.sin_port=htons(SERVER_PORT);
	//RICEVO DA QUALSIASI INTERFACCIA DELLA MACCHINA
	my_addr.sin_addr.s_addr=htonl(INADDR_ANY);

	//ERRNO 98, address already in use
	if(bind(ds_listen, (struct sockaddr*) &my_addr, sizeof(my_addr))==-1){
		printf("errore nella bind, se %d=98 c'è un bug\n", errno);
		raise(SIGILL);
	}
	if(listen(ds_listen,BACKLOG)==-1){
		puts("errore nella listen");
		raise(SIGILL);
	}
}


int main(){

	void* s;
	int your_addr_length;
	struct sockaddr_in your_addr;
	
	//PULISCO IL BUFFER DA EVENTUALI CHIUSURE ANOMALE	
	memset(toSend, 0, sizeof(toSend));
	bzero((char*)&your_addr,sizeof(your_addr));
	
	puts("inizio startup");
	sistemaSegnali();
	aperturaFile();
	restore();
	binding();
	alarm(ALARM);
	printf("startup finished... server up!\n");
	
	//INIZIO LAVORO !!!
	while(1){	
		while((ds_comunication=accept(ds_listen, (struct sockaddr*) &your_addr,&your_addr_length))==-1){
			//syscall bloccante che può essere abortita per arrivo di segnalazioni (in realtà l'unica di interesse è alarm)
			if(errno==EINTR)
				puts("aborted, re-posting");
			else{
				puts("errore non previsto. Exit");
				raise(SIGILL);
			}
		}
		if((pthread_create(&tid,0,gestisciSessione,0))!=0){
			puts("creazione del thread fallita");
			raise(SIGILL);
		}
		//questo join mi evita che nascano due o più thread
		pthread_join(tid,&s);
	}
	esci();
}
