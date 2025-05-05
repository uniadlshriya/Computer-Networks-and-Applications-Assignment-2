#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "sr.h"

/* ******************************************************************
   Selective Repeat.
**********************************************************************/

#define RTT  16.0       /* round trip time.  MUST BE SET TO 16.0 when submitting assignment */
#define WINDOWSIZE 6    /* the maximum number of buffered unacked packet */
#define SEQSPACE 7      /* the min sequence space for GBN must be at least windowsize + 1 */
#define NOTINUSE (-1)   /* used to fill header fields that are not being used */


/* generic procedure to compute the checksum of a packet.  Used by both sender and receiver  
   the simulator will overwrite part of your packet with 'z's.  It will not overwrite your 
   original checksum.  This procedure must generate a different checksum to the original if
   the packet is corrupted.
*/
int ComputeChecksum(struct pkt packet)
{
  int checksum = 0;
  int i;

  checksum = packet.seqnum;
  checksum += packet.acknum;
  for ( i=0; i<20; i++ ) 
    checksum += (int)(packet.payload[i]);

  return checksum;
}

bool IsCorrupted(struct pkt packet)
{
  if (packet.checksum == ComputeChecksum(packet))
    return (false);
  else
    return (true);
}


/********* Sender (A) variables and functions ************/
static struct pkt ABuffer[SEQSPACE]; /* array for storing packets waiting for ACK */
static float srSent[SEQSPACE];  /* Array for tracking sent message */
static bool srAcked[SEQSPACE];    /* adding an array to track each packet which are acknowledged (differs from GBN when they are cumulatively acked) */

static int base = 0; /* beginning of the sliding window */ 
static int A_nextseqnum = 0; /* the next sequence number to be used by the sender */



/*static struct pkt buffer[WINDOWSIZE];  array for storing packets waiting for ACK */
/*static int windowfirst, windowlast;    array indexes of the first/last packet awaiting ACK */
/*static int windowcount;                the number of packets currently awaiting an ACK */

/* called from layer 5 (application layer), passed the message to be sent to other side */

void A_output(struct msg message)
{
  struct pkt sendpkt;
  int i;
  int pckt_dropped = 0;

   /* if blocked,  window is full */
   /* wrap around circular buffer */
   if ( (A_nextseqnum-base+SEQSPACE) %SEQSPACE >= WINDOWSIZE) { 

    if (TRACE > 0)
      printf("----A: New message arrives, send window is full\n");
    pckt_dropped++;
    return;
   }
 
    if (TRACE > 1)
      printf("----A: New message arrives, send window is not full, send new message to layer3!\n");

    /* create packet */
    sendpkt.seqnum = A_nextseqnum;
    sendpkt.acknum = NOTINUSE;
    for ( i=0; i<20 ; i++ )
      sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt);

    /* put packet in window buffer */
   
    ABuffer[A_nextseqnum] = sendpkt;
    srAcked[A_nextseqnum] = false;
    srSent[A_nextseqnum] = true;
   

    /* send out packet */
    if (TRACE > 0)
      printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
    tolayer3 (A, sendpkt);

    /* start timer for this packet */
    if (base == A_nextseqnum)
       starttimer(A,RTT);

    /* get next sequence number */
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
}


/* called from layer 3, when a packet arrives for layer 4 
   In this practical this will always be an ACK as B never sends data.
*/
void A_input(struct pkt packet)
{
  /* if received ACK is not corrupted */ 
  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----A: uncorrupted ACK %d is received\n",packet.acknum);
    total_ACKs_received++;

    /* check packet for ACK*/
    if (!srAcked[packet.acknum]) {
       srAcked[packet.acknum] = true;
        /* packet is a new ACK */
        if (TRACE > 0)
           printf("----A: ACK %d is not a duplicate\n",packet.acknum);
        new_ACKs++;
       

        /* if the packed is acked then updated acked and sent */
        while(srAcked[base]) {
          srAcked[base] = false;
          srSent[base] = false;
          base = (base+1)%SEQSPACE;
          }
         stoptimer(A); /* Stop timer if they are acked, restart timer if it is any unacked packets remain */
         if (base != A_nextseqnum)
            starttimer(A, RTT);
        }
        else
          if (TRACE > 0)
        printf ("----A: duplicate ACK received, do nothing!\n");
  }
  else 
    if (TRACE > 0)
      printf ("----A: corrupted ACK is received, do nothing!\n");

  
}

/* called when A's timer goes off */
void A_timerinterrupt(void)
{
  int i;

  if (TRACE > 0)
    printf("----A: time out,resend packets!\n");

  for(i=0; i<SEQSPACE; i++) {
     if (srSent[i] && !srAcked[i]) {
        if (TRACE > 0)
            printf ("---A: resending packet %d\n", (ABuffer[i].seqnum));

        tolayer3(A,ABuffer[i]);
        packets_resent++;
        
    }
   }
  starttimer(A,RTT);
}       



/* the following routine will be called once (only) before any other */
/* entity A routines are called. You can use it to do any initialization */
void A_init(void)
{
  /* initialise A's window, base, Timers and packets  */
  int i;
  A_nextseqnum = 0;  /* A starts with seq num 0 */
  base = 0;
  for (i = 0; i < SEQSPACE; i++) {
       srAcked[i] = false; /* Intializing all packets to false */
  } 
}



/********* Receiver (B)  variables and procedures ************/
static struct pkt BBuffer[SEQSPACE]; /* array for storing received packets */
static bool recvpkt[SEQSPACE]; /* array to flag received packet */

static int expectedseqnum; /* the sequence number expected next by the receiver */
static int B_nextseqnum;   /* the sequence number for the next packets sent by B */


/* called from layer 3, when a packet arrives for layer 4 at B*/
void B_input(struct pkt packet)
{
  struct pkt sendpkt;
  int i;

  /* if not corrupted can receive outof order */
  if  (!IsCorrupted(packet)) {
    packets_received++;

    /* create packet */
    sendpkt.acknum = packet.seqnum;
    sendpkt.seqnum = NOTINUSE ; /* as A never reads seqnum from ACK */
    
    /* we don't have any data to send.  fill payload with 0's */
    for ( i=0; i<20 ; i++ ) 
      sendpkt.payload[i] = '0';  

    /* computer checksum */
    sendpkt.checksum = ComputeChecksum(sendpkt); 

    /* send out packet */
    tolayer3 (B, sendpkt);
    if (TRACE > 0)
      printf("----B: packet %d is correctly received, send ACK!\n",packet.seqnum);


    /* buffer the packets if they are not already buffered */

    if (!recvpkt[packet.seqnum]) {
      BBuffer[packet.seqnum] = packet;
      recvpkt[packet.seqnum] = true;
    }

    
    /* deliver to receiving application  all in-order packets*/
    while(recvpkt[expectedseqnum]) {
     tolayer5(B, BBuffer[expectedseqnum].payload);
     recvpkt[expectedseqnum] = false;
     /* update state variables */
     expectedseqnum = (expectedseqnum + 1) % SEQSPACE;  
    }      
  }
  else {
    /* packet is corrupted or out of order resend last ACK */
    if (TRACE > 0) 
      printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
  }
}

/* the following routine will be called once (only) before any other */
/* entity B routines are called. You can use it to do any initialisation */
void B_init(void)
{
  int i;
  expectedseqnum = 0;
  B_nextseqnum = 1;

  for ( i=0; i< SEQSPACE; i++) {
    recvpkt[i] = false;
  }
 
}

/******************************************************************************
 * The following functions need be completed only for bi-directional messages *
 *****************************************************************************/

/* Note that with simplex transfer from a-to-B, there is no B_output() */
void B_output(struct msg message)  
{
}

/* called when B's timer goes off */
void B_timerinterrupt(void)
{
}

