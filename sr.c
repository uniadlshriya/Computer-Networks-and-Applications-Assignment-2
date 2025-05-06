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
#define SEQSPACE 12      /* the min sequence space for GBN must be at least windowsize 2n */
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
static bool srAcked[WINDOWSIZE];    /* adding an array to track each packet which are acknowledged (differs from GBN when they are cumulatively acked) */
static int A_nextseqnum = 0; /* the next sequence number to be used by the sender */


static struct pkt buffer[WINDOWSIZE];  /* array for storing packets waiting for ACK */
static int windowfirst, windowlast;    /* array indexes of the first/last packet awaiting ACK */
static int windowcount;                /* the number of packets currently awaiting an ACK */

/* called from layer 5 (application layer), passed the message to be sent to other side */
void A_output(struct msg message)
{
  struct pkt sendpkt;
  int i;

  /* if not blocked waiting on ACK */
  if ( windowcount < WINDOWSIZE) {
    if (TRACE > 1)
      printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    /* create packet */
    sendpkt.seqnum = A_nextseqnum;
    sendpkt.acknum = NOTINUSE;
    for ( i=0; i<20 ; i++ ) 
      sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt); 

    /* put packet in window buffer */
    /* windowlast will always be 0 for alternating bit; but not for GoBackN */
    windowlast = (windowlast + 1) % WINDOWSIZE; 
    buffer[windowlast] = sendpkt;
    srAcked[windowlast] = false;
    windowcount++;

    /* send out packet */
    if (TRACE > 0)
      printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
    tolayer3 (A, sendpkt);

    /* start timer if first packet in window */
    if (windowcount == 1)
      starttimer(A,RTT);

    /* get next sequence number, wrap back to 0 */
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;  
  }
  /* if blocked,  window is full */
  else {
    if (TRACE > 0)
      printf("----A: New message arrives, send window is full\n");
    window_full++;
  }
}


/* called from layer 3, when a packet arrives for layer 4 
   In this practical this will always be an ACK as B never sends data.
*/
void A_input(struct pkt packet)
{
  int i=0;
  int id;
  bool wslide = false;
  /* if received ACK is not corrupted */ 
  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----A: uncorrupted ACK %d is received\n",packet.acknum);
    
    total_ACKs_received++;

    /* check packet for ACK*/
    while(i<windowcount) {
      /* looping though the packets in the window */
      id = (windowfirst + i) % WINDOWSIZE;
      if (buffer[id].seqnum == packet.acknum) {
         if (!srAcked[id])  { /* checking for duplicate Acks */
           if (TRACE > 0)
             printf("----A: ACK %d is not a duplicate\n",packet.acknum);
            new_ACKs++; 
            srAcked[id] = true;
          } else {
           if (TRACE > 0)
             printf ("----A: duplicate ACK received, do nothing!\n");
         }
        break; /* Ack is matched do not continue */
      }
      i++;
    }

    /* slide window for consecutive acks */
    while(srAcked[windowfirst] && (windowcount >0)) {
       srAcked[windowfirst] = false;
       windowfirst = (windowfirst +1) % WINDOWSIZE;
       wslide = true; /* slide window */
       windowcount--;
     }
     
    /* start timer again if there are still more unacked packets in window */
    if (wslide) {
       stoptimer(A);
       if (windowcount > 0)
          starttimer(A, RTT);
    }
  }
  else {
    if (TRACE > 0)
      printf ("----A: corrupted ACK is received, do nothing!\n");
  } 
}

/* called when A's timer goes off */
void A_timerinterrupt(void)
{
  int i=0;
  int id;

  if (TRACE > 0)
    printf("----A: time out,resend packets!\n");

 /* similar to A_input checking for unacked packed to resend */
  while(i<windowcount) {
    id = (windowfirst+1)%WINDOWSIZE;
    if(!srAcked[id]) {
      tolayer3(A, buffer[id]);
      if (TRACE > 0)
       printf ("---A: resending packet %d\n", buffer[id].seqnum);
     packets_resent++;
     starttimer(A,RTT);
     break;  
    }
    i++;     
  }

}       



/* the following routine will be called once (only) before any other */
/* entity A routines are called. You can use it to do any initialization */
void A_init(void)
{
  /* initialise A's window, base, Timers and packets  */
  int i;
  A_nextseqnum = 0;  /* A starts with seq num 0 */
  windowfirst = 0;
  windowlast = -1;   /* windowlast is where the last packet sent is stored.  
		     new packets are placed in winlast + 1 
		     so initially this is set to -1
		   */
  windowcount = 0;

  for (i = 0; i < SEQSPACE; i++) {
       srAcked[i] = false; /* Intializing all packets to false */
  } 
}



/********* Receiver (B)  variables and procedures ************/
static struct pkt recvBuffer[SEQSPACE]; /* array for storing received packets */
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

    if (TRACE > 0)
      printf("----B: packet %d is correctly received, send ACK!\n",packet.seqnum);

    if(!recvpkt[packet.seqnum]) {
      recvpkt[packet.seqnum] = true;
      recvBuffer[packet.seqnum] = packet;
      packets_received++;

      /* Deliver in-order packets */
      while(recvpkt[expectedseqnum]) {
       tolayer5(B, recvBuffer[expectedseqnum].payload);
       recvpkt[expectedseqnum] = false;
       /* update state variables */
       expectedseqnum = (expectedseqnum + 1) % SEQSPACE;  
      } 
     }   
     sendpkt.acknum = packet.seqnum;
  }
  else {
    /* packet is corrupted or out of order resend last ACK */
    if (TRACE > 0) 
      printf("----B: packet corrupted or not expected sequence number, resend ACK!\n");
    if (expectedseqnum == 0)
      sendpkt.acknum = SEQSPACE - 1;
    else
      sendpkt.acknum = expectedseqnum - 1;
  }
  /* create packet */
  sendpkt.seqnum = B_nextseqnum;
  B_nextseqnum = (B_nextseqnum + 1) % 2;
    
  /* we don't have any data to send.  fill payload with 0's */
  for ( i=0; i<20 ; i++ ) 
    sendpkt.payload[i] = '0';  

  /* computer checksum */
  sendpkt.checksum = ComputeChecksum(sendpkt); 

  /* send out packet */
  tolayer3 (B, sendpkt);

}

/* the following routine will be called once (only) before any other */
/* entity B routines are called. You can use it to do any initialization */
void B_init(void)
{
  int i;
  expectedseqnum = 0;
  B_nextseqnum = 1;
  for (i=0; i< SEQSPACE; i++) {
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

