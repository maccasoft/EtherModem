
#include <EEPROM.h>
#include <UIPEthernet.h>
#include <Dns.h>

uint8_t mac[6] = {
  0x00,0x01,0x02,0x03,0x04,0x05
};

EthernetClient modemClient;

unsigned long lastConnectCheck = 0;
unsigned long prevCharTime = 0;
unsigned long prevRingTime = 0;
uint8_t modemEscapeState = 0;
bool    modemCommandMode = true;


const PROGMEM uint8_t regDefaults[] = {2, '+', 
                               3, '\r', 
                               4, '\n', 
                               5, 8, 
                               6, 2, 
                               7, 50, 
                               8, 2, 
                               9, 6, 
                               10, 14, 
                               11, 95, 
                               12, 50, 
                               25, 5,
                               38, 20};


const PROGMEM int linespeeds[] = {0, 75, 110, 300, 600, 1200, 2400, 4800, 7200, 9600, 12000, 14400};
#define NSPEEDS (sizeof(linespeeds)/sizeof(int))

#define LED_PIN            2
#define DCD_PIN            2

#define REG_ESC            2
#define REG_CR             3
#define REG_LF             4
#define REG_BSP            5
#define REG_GUARDTIME     12
#define REG_TELNET        15
#define REG_LINESPEED     37
#define REG_CURLINESPEED  43
#define REG_WIFI_CHANNEL  99

#define RING_MILLIS       4500

enum
  {
    E_OK = 0,
    E_CONNECT,
    E_RING,
    E_NOCARRIER,
    E_ERROR,
    E_CONNECT1200,
    E_NODIALTONE,
    E_BUSY,
    E_NOANSWER,
    E_CONNECT600,
    E_CONNECT2400,
    E_CONNECT4800,
    E_CONNECT9600,
    E_CONNECT14400,
    E_CONNECT19200
  };


struct TelnetStateStruct
{
  uint8_t cmdLen;
  uint8_t cmd[4];
  bool sendBinary;
  bool receiveBinary;
  bool receivedCR;
  bool subnegotiation;
} modemTelnetState, clientTelnetState;


#define MAGICVAL 0xF0E1D2C3B4A59687

struct ModemDataStruct
{
  uint64_t magic;

  uint32_t baud;
  byte     bits;
  byte     parity;
  byte     stopbits;
  byte     silent;
  byte     unused;
  char     telnetTerminalType[32];

  uint8_t  reg[32];

  uint8_t  extCodes;
  uint8_t  echo;
  uint8_t  quiet;
  uint8_t  verbose;
} ModemData;


void applySerialSettings()
{
  Serial.flush();
  Serial.end();
  delay(100);
  Serial.begin(ModemData.baud);
}


void clearSerialBuffer()
{
  // empty the serial buffer
  delay(100); 
  while( Serial.available()>0 ) { Serial.read(); delay(10); }
}


void setup()
{
  EEPROM.get(0, ModemData);
  if( ModemData.magic != MAGICVAL )
    {
      // use default settings
      memset(&ModemData, 0, sizeof(ModemData));
      ModemData.magic    = MAGICVAL;
      ModemData.baud     = 9600;
      ModemData.bits     = 8;
      ModemData.parity   = 0;
      ModemData.stopbits = 1;
      ModemData.silent   = false;
      strcpy(ModemData.telnetTerminalType, PSTR("vt100"));
      for(int i=0; i<sizeof(regDefaults); i+=2)
        {
          ModemData.reg[regDefaults[i]] = regDefaults[i+1];
        }
      ModemData.extCodes = 0;
      ModemData.echo = true;
      ModemData.quiet = false;
      ModemData.verbose = true;
    }

  Serial.begin(ModemData.baud);
  Ethernet.begin(mac);

  resetModemState();

  // flush serial input buffer
  while( Serial.available() ) Serial.read();

  printModemResult(E_OK);
}


void resetTelnetState(struct TelnetStateStruct &s)
{
  s.cmdLen = 0;
  s.sendBinary = false;
  s.receiveBinary = false;
  s.receivedCR = false;
  s.subnegotiation = false;
}


void resetModemState()
{
  modemEscapeState = 0;
  modemCommandMode = true;

  if( modemClient.connected() )
    {
      modemClient.stop();
      //digitalWrite(DCD_PIN, LOW);
    }
}


void printModemCR()
{
  Serial.print(char(ModemData.reg[REG_CR]));
  if( ModemData.verbose ) Serial.print(char(ModemData.reg[REG_LF]));
}


void printMac()
{
  int i;
  for (i = 0; i < 6; i++)
    {
      if (i != 0) Serial.print(F(":"));
      if (mac[i] < 16) Serial.print(F("0"));
      Serial.print(mac[i], HEX);
    }
}


void printModemResult(byte code)
{
  if( !ModemData.quiet )
    {
      if( ModemData.verbose )
        {
          printModemCR();
          switch( code )
            {
            case E_OK            : Serial.print(F("OK"));             break;
            case E_CONNECT       : Serial.print(F("CONNECT"));        break;
            case E_RING          : Serial.print(F("RING"));           break;
            case E_NOCARRIER     : Serial.print(F("NO CARRIER"));     break;
            case E_ERROR         : Serial.print(F("ERROR"));          break;
            case E_CONNECT1200   : Serial.print(F("CONNECT 1200"));   break;
            case E_NODIALTONE    : Serial.print(F("NO DIALTONE"));    break;
            case E_BUSY          : Serial.print(F("BUSY"));           break;
            case E_NOANSWER      : Serial.print(F("NO ANSWER"));      break;
            case E_CONNECT600    : Serial.print(F("CONNECT 600"));    break;
            case E_CONNECT2400   : Serial.print(F("CONNECT 2400"));   break;
            case E_CONNECT4800   : Serial.print(F("CONNECT 4800"));   break;
            case E_CONNECT9600   : Serial.print(F("CONNECT 9600"));   break;
            case E_CONNECT14400  : Serial.print(F("CONNECT 14400"));  break;
            default:
              {
                char buf[20];
                sprintf(buf, PSTR("ERROR%i"), code);
                Serial.print(buf);
                break;
              }
            }
        }
      else
        Serial.print(code);

      printModemCR();
    }
}


long getCmdParam(char *cmd, int &ptr)
{
  long res = 0;

  ptr++;
  while( isdigit(cmd[ptr]) )
    {
      res = (res * 10) + (cmd[ptr] - '0');
      ptr++;
    }

  return res;
}


void getCmdParam(char *cmd, char *res, int &ptr)
{
  ptr++;
  if( cmd[ptr]=='"' )
    {
      ptr++;
      while( cmd[ptr]!=0 && cmd[ptr]!='"' )
        {
          *res++ = cmd[ptr++];
        }
      if( cmd[ptr]=='"' )
          ptr++;
    }

  *res = 0;
}


// returns TRUE if input byte "b" is part of a telnet protocol sequence and should
// NOT be passed through to the serial interface
bool handleTelnetProtocol(uint8_t b, EthernetClient &client, struct TelnetStateStruct &state)
{
#define T_SE      240 // 0xf0
#define T_NOP     241 // 0xf1
#define T_BREAK   243 // 0xf3
#define T_GOAHEAD 249 // 0xf9
#define T_SB      250 // 0xfa
#define T_WILL    251 // 0xfb
#define T_WONT    252 // 0xfc
#define T_DO      253 // 0xfd
#define T_DONT    254 // 0xfe
#define T_IAC     255 // 0xff

#define TO_SEND_BINARY        0
#define TO_ECHO               1
#define TO_SUPPRESS_GO_AHEAD  3
#define TO_TERMINAL_TYPE      24

  // if not handling telnet protocol then just return
  if( !ModemData.reg[REG_TELNET] ) return false;

  // ---- handle incoming sub-negotiation sequences

  if( state.subnegotiation )
    {
      if( ModemData.reg[REG_TELNET]>1 ) {Serial.print('<'); Serial.print(b, HEX);}

      if( state.cmdLen==0 && b == T_IAC )
        state.cmdLen = 1; 
      else if( state.cmdLen==1 && b == T_SE )
        {
          state.subnegotiation = false;
          state.cmdLen = 0;
        }
      else
        state.cmdLen = 0;

      return true;
    }

  // ---- handle CR-NUL sequences

  if( state.receivedCR )
    {
      state.receivedCR = false;
      if( b==0 )
        {
          // CR->NUL => CR (i.e. ignore the NUL)
          if( ModemData.reg[REG_TELNET]>1 ) Serial.print(F("<0d<00"));
          return true;
        }
    }
  else if( b == 0x0d && state.cmdLen==0 && !state.receiveBinary )
    {
      // received CR while not in binary mode => remember but pass through
      state.receivedCR = true;
      return false;
    }

  // ---- handle IAC sequences

  if( state.cmdLen==0 )
    {
      // waiting for IAC byte
      if( b == T_IAC )
        {
          state.cmdLen = 1;
          state.cmd[0] = T_IAC;
          if( ModemData.reg[REG_TELNET]>1 ) {Serial.print('<'); Serial.print(b, HEX);}
          return true;
        }
    }
  else if( state.cmdLen==1 )
    {
      if( ModemData.reg[REG_TELNET]>1 ) {Serial.print('<'); Serial.print(b, HEX);}
      // received second byte of IAC sequence
      if( b == T_IAC )
        {
          // IAC->IAC sequence means regular 0xff data value
          state.cmdLen = 0; 

          // we already skipped the first IAC (0xff), so just return 'false' to pass this one through
          return false; 
        }
      else if( b == T_NOP || b == T_BREAK || b == T_GOAHEAD )
        {
          // NOP, BREAK, GOAHEAD => do nothing and skip
          state.cmdLen = 0; 
          return true; 
        }
      else if( b == T_SB )
        {
          // start of sub-negotiation
          state.subnegotiation = true;
          state.cmdLen = 0;
          return true;
        }
      else
        {
          // record second byte of sequence
          state.cmdLen = 2;
          state.cmd[1] = b;
          return true;
        }
    }
  else if( state.cmdLen==2 )
    {
      // received third (i.e. last) byte of IAC sequence
      if( ModemData.reg[REG_TELNET]>1 ) {Serial.print('<'); Serial.print(b, HEX);}
      state.cmd[2] = b;

      bool reply = true;
      if( state.cmd[1] == T_WILL )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY:        state.cmd[1] = T_DO; state.receiveBinary = true; break;
            case TO_ECHO:               state.cmd[1] = T_DO; break;
            case TO_SUPPRESS_GO_AHEAD:  state.cmd[1] = T_DO; break;
            default: state.cmd[1] = T_DONT; break;
            }
        }
      else if( state.cmd[1] == T_WONT )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY: state.cmd[1] = T_DO; state.receiveBinary = false; break;
            }
          state.cmd[1] = T_DONT; 
        }
      else if( state.cmd[1] == T_DO )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY:       state.cmd[1] = T_WILL; state.sendBinary = true; break;
            case TO_SUPPRESS_GO_AHEAD: state.cmd[1] = T_WILL; break;
            case TO_TERMINAL_TYPE:     state.cmd[1] = ModemData.telnetTerminalType[0]==0 ? T_WONT : T_WILL; break;
            default: state.cmd[1] = T_WONT; break;
            }
        }
      else if( state.cmd[1] == T_DONT )
        {
          switch( state.cmd[2] )
            {
            case TO_SEND_BINARY: state.cmd[1] = T_WILL; state.sendBinary = false; break;
            }
          state.cmd[1] = T_WONT;
        }
      else
        reply = false;

      // send reply if necessary
      if( reply ) 
        {
          if( ModemData.reg[REG_TELNET]>1 )
            for(int k=0; k<3; k++) {Serial.print('>'); Serial.print(state.cmd[k], HEX);}

          client.write(state.cmd, 3);

          if( state.cmd[1] == T_WILL && state.cmd[2] == TO_TERMINAL_TYPE )
            {
              // send terminal-type subnegoatiation sequence
              uint8_t buf[110], i, n=0;
              buf[n++] = T_IAC;
              buf[n++] = T_SB;
              buf[n++] = TO_TERMINAL_TYPE;
              buf[n++] = 0; // IS
              for(i=0; i<100 && ModemData.telnetTerminalType[i]>=32 && ModemData.telnetTerminalType[i]<127; i++) 
                buf[n++] = ModemData.telnetTerminalType[i];
              buf[n++] = T_IAC;
              buf[n++] = T_SE;
              client.write(buf, n);
              if( ModemData.reg[REG_TELNET]>1 )
                for(int k=0; k<n; k++) {Serial.print('>'); Serial.print(buf[k], HEX);}
            }
        }

      // start over
      state.cmdLen = 0;
      return true;
    }
  else
    {
      // invalid state (should never happen) => just reset
      state.cmdLen = 0;
    }

  return false;
}


int strncmpi(const char *s1, const char *s2, size_t cchars)
{
  char c1, c2;

  if ( s1==s2 )
    return 0;

  if ( s1==0 )
    return -1;

  if ( s2==0 )
    return 1;

  if ( cchars==0 )
    return 0;

  do {
    c1 = toupper(*s1);
    c2 = toupper(*s2);
    s1++;
    s2++;
    cchars--;
  } while ( (c1 != 0) && (c1 == c2) && (cchars>0) );

  return (int)(c1 - c2);
}


int getConnectStatus()
{
  int res;

  if( ModemData.reg[REG_LINESPEED]==0 )
    {
      int i = 0;
      while( i<NSPEEDS && linespeeds[i]<ModemData.baud ) i++;
      if( i==NSPEEDS )
        ModemData.reg[REG_CURLINESPEED] = 255;
      else if( linespeeds[i]==ModemData.baud )
        ModemData.reg[REG_CURLINESPEED] = i;
      else
        ModemData.reg[REG_CURLINESPEED] = i-1;
    }
  else if( ModemData.reg[REG_LINESPEED] < NSPEEDS )
    ModemData.reg[REG_CURLINESPEED] = min(ModemData.reg[REG_LINESPEED], byte(NSPEEDS-1));

  if( ModemData.extCodes==0 )
    {
      res = E_CONNECT;
    }
  else
    {
      switch( ModemData.reg[REG_CURLINESPEED] )
        {
        case 3: res = E_CONNECT; break;
        case 4: res = E_CONNECT600; break;
        case 5: res = E_CONNECT1200; break;
        case 6: res = E_CONNECT2400; break;
        case 7: res = E_CONNECT4800; break;
        case 9: res = E_CONNECT9600; break;
        default: res = E_CONNECT; break;
        }
    }

    return res;
}


void handleModemCommand()
{
  // check if a modem AT command was received
  static int cmdLen = 0, prevCmdLen = 0;
  static char cmd[81];
  char c = 0;

  if( Serial.available() )
    {
      c = Serial.read() & 0x7f;
      if( cmdLen<80 && c>32 )
        cmd[cmdLen++] = c;
      else if( cmdLen>0 && c == ModemData.reg[REG_BSP] )
        cmdLen--;

      if( ModemData.echo ) 
        {
          if( c == ModemData.reg[REG_BSP] )
            { Serial.print(char(8)); Serial.print(' '); Serial.print(char(8)); }
          else
            Serial.print(c);
        }

      prevCharTime = millis();
    }

  if( cmdLen==2 && toupper(cmd[0])=='A' && cmd[1]=='/' )
    {
      c = ModemData.reg[REG_CR];
      cmd[1] = 'T';
      cmdLen = prevCmdLen;
    }

  if( c == ModemData.reg[REG_CR] )
    {
      while( (millis()-prevCharTime)<100 )
        {
          if( Serial.available() )
            {
              Serial.read();
              break;
            }
        }

      prevCmdLen = cmdLen;
      if( cmdLen>=2 && toupper(cmd[0])=='A' && toupper(cmd[1])=='T' )
        {
          cmd[cmdLen]=0;
          int ptr = 2;
          bool connecting = false;
          int status = E_OK;
          while( status!=E_ERROR && ptr<cmdLen )
            {
              if( toupper(cmd[ptr])=='D' )
                {
                  unsigned long t = millis();

                  // if we are already connected then disconnect first
                  if( modemClient.connected() ) 
                    {
                      modemClient.stop();
                      ModemData.reg[REG_CURLINESPEED] = 0;
                    }

                  ptr++;
                  if( toupper(cmd[ptr])=='T' || toupper(cmd[ptr])=='P' )
                    {
                      ptr++;
                    }

                  bool haveAlpha = false;
                  int numSep = 0;
                  for(int j=ptr; j<cmdLen && !haveAlpha; j++)
                    {
                      if( isalpha(cmd[j]) )
                        haveAlpha = true;
                      else if( !isdigit(cmd[j]) )
                        {
                          numSep++;
                          while( j<cmdLen && !isdigit(cmd[j]) ) j++;
                        }
                    }

                  byte n[4];
                  IPAddress addr;
                  int port = 23;
                  if( haveAlpha )
                    {
                      int p = ptr;
                      while( cmd[p]!=':' && p<cmdLen ) p++;
                      char c = cmd[p];
                      cmd[p] = 0;

                      DNSClient dns;
                      dns.begin(Ethernet.dnsServerIP());
                      if (dns.getHostByName(cmd+ptr, addr) == 1)
                        {
                          if( p+1<cmdLen ) port = atoi(cmd+p+1);
                        }
                      else
                        status = E_NOCARRIER;
                      cmd[p] = c;
                    }
                  else if( numSep==0 )
                    {
                      if( (cmdLen-ptr==12) || (cmdLen-ptr==16) )
                        {
                          for(int j=0; j<12; j+=3)
                            {
                              char c = cmd[ptr+j+3];
                              cmd[ptr+j+3] = 0;
                              n[j/3] = atoi(cmd+ptr+j);
                              cmd[ptr+j+3] = c;
                            }

                          addr = IPAddress(n);
                          if( cmdLen-ptr==16 ) port = atoi(cmd+ptr+12);
                        }
                      else
                        status = E_ERROR;

                      ptr = cmdLen;
                    }
                  else if( numSep==3 || numSep==4 )
                    {
                      for(int j=0; j<4; j++)
                        {
                          int p = ptr;
                          while( isdigit(cmd[ptr]) && ptr<cmdLen ) ptr++;
                          char c = cmd[ptr];
                          cmd[ptr] = 0;
                          n[j] = atoi(cmd+p);
                          cmd[ptr] = c;
                          while( !isdigit(cmd[ptr]) && ptr<cmdLen ) ptr++;
                        }
                      addr = IPAddress(n);
                      if( numSep==4 ) port = atoi(cmd+ptr);
                    }
                  else
                    status = E_ERROR;

                  if( status==E_OK )
                    {
                      if( modemClient.connect(addr, port) )
                        {
                          modemCommandMode = false;
                          modemEscapeState = 0;
                          resetTelnetState(modemTelnetState);

                          status = getConnectStatus();
                          connecting = true;
                        }
                      else if( ModemData.extCodes==0 )
                        status = E_NOCARRIER;
                      else if( Ethernet.linkStatus() != LinkON )
                        status = E_NODIALTONE;
                      else
                        status = E_NOANSWER;

                      // force at least 1 second delay between receiving
                      // the dial command and responding to it
                      if( millis()-t<1000 ) delay(1000-(millis()-t));
                    }

                  // ATD can not be followed by other commands
                  ptr = cmdLen;
                }
              else if( toupper(cmd[ptr])=='A' )
                {
                  // ATA can not be followed by other commands
                  ptr = cmdLen;
                }
              else if( toupper(cmd[ptr])=='H' )
                {
                  if( cmdLen-ptr==1 || toupper(cmd[ptr+1])=='0' )
                    {
                      // hang up
                      if( modemClient && modemClient.connected() ) 
                        {
                          modemClient.stop();
                        }
                      ModemData.reg[REG_CURLINESPEED] = 0;
                    }

                  ptr += 2;
                }
              else if( toupper(cmd[ptr])=='O' )
                {
                  getCmdParam(cmd, ptr);
                  if( modemClient && modemClient.connected() )
                    {
                      modemCommandMode = false;
                      modemEscapeState = 0;
                      break;
                    }
                }
              else if( toupper(cmd[ptr])=='E' )
                ModemData.echo = getCmdParam(cmd, ptr)!=0;
              else if( toupper(cmd[ptr])=='Q' )
                ModemData.quiet = getCmdParam(cmd, ptr)!=0;
              else if( toupper(cmd[ptr])=='V' )
                ModemData.verbose = getCmdParam(cmd, ptr)!=0;
              else if( toupper(cmd[ptr])=='X' )
                ModemData.extCodes = getCmdParam(cmd, ptr);
              else if( toupper(cmd[ptr])=='Z' )
                {
                  // reset serial settings to saved value
                  EEPROM.get(0, ModemData);
                  applySerialSettings();

                  // reset parameters (ignore command parameter)
                  getCmdParam(cmd, ptr);
                  resetModemState();

                  // ATZ can not be followed by other commands
                  ptr = cmdLen;
                }
              else if( toupper(cmd[ptr])=='S' )
                {
                  static uint8_t currentReg = 0;
                  int p = ptr;
                  int reg = getCmdParam(cmd, ptr);
                  if( ptr==p+1 ) reg = currentReg;
                  currentReg = reg;

                  if( cmd[ptr]=='?' )
                    {
                      ptr++;
                      byte v = ModemData.reg[reg];
                      if( ModemData.verbose ) printModemCR();
                      if( v<100 ) Serial.print('0');
                      if( v<10  ) Serial.print('0');
                      Serial.print(v);
                      printModemCR();
                    }
                  else if( cmd[ptr]=='=' )
                    {
                      byte v = getCmdParam(cmd, ptr);
                      if( reg != REG_CURLINESPEED )
                        {
                          ModemData.reg[reg] = v;
                        }
                    }
                }
              else if( toupper(cmd[ptr])=='I' )
                {
                  byte n = getCmdParam(cmd, ptr);
                  if( n == 0 )
                    {
                      printModemCR();
                      Serial.print(F("Ethernet/"));
                      switch(Ethernet.hardwareStatus())
                        {
                          case EthernetW5100:
                            Serial.print(F("W5100"));
                            break;
                          case EthernetW5200:
                            Serial.print(F("W5200"));
                            break;
                          case EthernetW5500:
                            Serial.print(F("W5500"));
                            break;
                          case EthernetENC28J60:
                            Serial.print(F("ENC28J60"));
                            break;
                          default:
                            Serial.print(F("Unknown"));
                            break;
                        }
                      Serial.print(F(" Modem 1.0"));
                      printModemCR();
                      Serial.print(F("IP="));
                      Serial.print(Ethernet.localIP());
                      printModemCR();
                      Serial.print(F("MAC="));
                      printMac();
                    }
                  else if( n == 1 || n == 5 )
                    {
                      bool showAll = (n == 5);
                      printModemCR();
                      Serial.print(F("AT"));
                      Serial.print(ModemData.echo ? F("E1") : F("E0"));
                      if( ModemData.quiet )
                        {
                          Serial.print(F("Q1"));
                          if( showAll )
                          {
                            Serial.print(ModemData.verbose ? F("V1") : F("V0"));
                            Serial.print(ModemData.extCodes ? F("X1") : F("X0"));
                          }
                        }
                      else
                        {
                          Serial.print(F("Q0"));
                          Serial.print(ModemData.verbose ? F("V1") : F("V0"));
                          Serial.print(ModemData.extCodes ? F("X1") : F("X0"));
                        }
                      Serial.print(F("S0="));
                      Serial.print(ModemData.reg[0]);
                      if( ModemData.reg[REG_ESC] != '+' || showAll )
                        {
                          Serial.print(F("S2="));
                          Serial.print(ModemData.reg[REG_ESC]);
                        }
                      if( ModemData.reg[REG_CR] != 13 || showAll )
                        {
                          Serial.print(F("S3="));
                          Serial.print(ModemData.reg[REG_CR]);
                        }
                      if( ModemData.reg[REG_LF] != 10 || showAll )
                        {
                          Serial.print(F("S4="));
                          Serial.print(ModemData.reg[REG_LF]);
                        }
                      if( ModemData.reg[REG_BSP] != 8 || showAll )
                        {
                          Serial.print(F("S5="));
                          Serial.print(ModemData.reg[REG_BSP]);
                        }
                      if( ModemData.reg[REG_TELNET] != 0 || showAll )
                        {
                          Serial.print(F("S15="));
                          Serial.print(ModemData.reg[REG_TELNET]);
                        }
                    }
                  else if( n == 2 )
                    {
                      printModemCR();
                      Serial.print(Ethernet.localIP());
                    }
                  else if( n == 4 )
                    {
                      printModemCR();
                      Serial.print(F("1.0"));
                    }
                  else if( n == 6 )
                    {
                      printModemCR();
                      printMac();
                    }
                  else
                    status = E_ERROR;
                }
              else if( toupper(cmd[ptr])=='M' || toupper(cmd[ptr])=='L' || toupper(cmd[ptr])=='P' || toupper(cmd[ptr])=='T' )
                {
                  // ignore speaker settings, answer requests, pulse/tone dial settings
                  getCmdParam(cmd, ptr);
                }
              else if( cmd[ptr]=='&' )
                {
                  ptr++;
                  if( toupper(cmd[ptr])=='W' )
                    {
                      getCmdParam(cmd, ptr);
                      EEPROM.put(0, ModemData);
                    }
                  else if( toupper(cmd[ptr])=='F')
                    {
                      // use default settings
                      memset(&ModemData, 0, sizeof(ModemData));
                      ModemData.magic    = MAGICVAL;
                      ModemData.baud     = 9600;
                      ModemData.bits     = 8;
                      ModemData.parity   = 0;
                      ModemData.stopbits = 1;
                      ModemData.silent   = false;
                      strcpy(ModemData.telnetTerminalType, "vt100");
                      for(int i=0; i<sizeof(regDefaults); i+=2)
                        {
                          ModemData.reg[regDefaults[i]] = regDefaults[i+1];
                        }
                      ModemData.extCodes = 0;
                      ModemData.echo = true;
                      ModemData.quiet = false;
                      ModemData.verbose = true;

                      applySerialSettings();

                      // reset parameters (ignore command parameter)
                      getCmdParam(cmd, ptr);
                      resetModemState();

                      // can not be followed by other commands
                      ptr = cmdLen;
                    }
                  else
                    {
                      status = E_ERROR;
                      ptr = cmdLen;
                    }
                }
              else if( cmd[ptr]=='+' )
                {
                  ptr++;
                  if( strncmpi(cmd+ptr, PSTR("UART"), 4)==0 )
                    {
                      ptr += 4;
                      int i = getCmdParam(cmd, ptr);
                      if ( i!=0 )
                        {
                          ModemData.baud = i;
                          if( cmd[ptr]==',' )
                            {
                              i = getCmdParam(cmd, ptr);
                              ModemData.bits = i;
                            }
                          if( cmd[ptr]==',' )
                            {
                              i = getCmdParam(cmd, ptr);
                              ModemData.stopbits = i;
                            }
                          if( cmd[ptr]==',' )
                            {
                              i = getCmdParam(cmd, ptr);
                              ModemData.parity = i;
                            }
                          if( cmd[ptr]==',' )
                            {
                              getCmdParam(cmd, ptr);
                              // Ignore flow control
                            }
                          printModemResult(E_OK);
                          applySerialSettings();
                          status = -1;
                        }
                      else
                        status = E_ERROR;
                    }
                  else
                    status = E_ERROR;
                  ptr = cmdLen;
                }
              else
                status = E_ERROR;
            }

          if( status>=0 )
            printModemResult(status);

          // delay 1 second after a "CONNECT" message
          if( connecting ) delay(1000);
        }
      else if( cmdLen>0 )
        printModemResult(E_ERROR);

      cmdLen = 0;
    }
}


void relayModemData()
{
  if( modemClient && modemClient.connected() && modemClient.available() ) 
    {
      int baud = ModemData.reg[REG_CURLINESPEED]==255 ? ModemData.baud : linespeeds[ModemData.reg[REG_CURLINESPEED]];

      if( baud == ModemData.baud )
        {
          // use full modem<->computer data rate
          unsigned long t = millis();
          while( modemClient.available() && Serial.availableForWrite() && millis()-t < 100 )
            {
              uint8_t b = modemClient.read();
              if( !handleTelnetProtocol(b, modemClient, modemTelnetState) ) Serial.write(b);
            }
        }
      else if( modemClient.available() && Serial.availableForWrite() )
        {
          // limit data rate
          static unsigned long nextChar = 0;
          if( millis()>=nextChar )
            {
              uint8_t b = modemClient.read();
              if( !handleTelnetProtocol(b, modemClient, modemTelnetState) )
                {
                  Serial.write(b);
                  nextChar = millis() + 10000/baud;
                }
            }
        }
    }

  if( millis() > prevCharTime + 20*ModemData.reg[REG_GUARDTIME] )
    {
      if( modemEscapeState==0 )
        modemEscapeState = 1;
      else if( modemEscapeState==4 )
        {
          // received [1 second pause] +++ [1 second pause]
          // => switch to command mode
          modemCommandMode = true;
          printModemResult(E_OK);
        }
    }

  if( Serial.available() )
    {
      uint8_t buf[128];
      int n = 0, millisPerChar = 1000 / (ModemData.baud / (1+ModemData.bits+ModemData.stopbits)) + 1;
      unsigned long startTime = millis();

      if( millisPerChar<5 ) millisPerChar = 5;
      while( Serial.available() && n<sizeof(buf) && millis()-startTime < 100 )
        {
          uint8_t b = Serial.read();

          if( ModemData.reg[REG_TELNET] )
            {
              // Telnet protocol handling is enabled

              // must duplicate IAC tokens
              if( b==T_IAC ) buf[n++] = b;

              // if not sending in binary mode then a stand-alone CR (without LF) must be followd by NUL
              if( !modemTelnetState.sendBinary && n>0 && buf[n-1] == 0x0d && b != 0x0a ) buf[n++] = 0;
            }

          buf[n++] = b;
          prevCharTime = millis();

          if( modemEscapeState>=1 && modemEscapeState<=3 && b==ModemData.reg[REG_ESC] )
            modemEscapeState++;
          else
            modemEscapeState=0;

          // wait a short time to see if another character is coming in so we
          // can send multi-character (escape) sequences in the same packet
          // some BBSs don't recognize the sequence if there is too much delay
          while( !Serial.available() && millis()-prevCharTime < millisPerChar );
        }

      // if not sending in binary mode then a stand-alone CR (without LF) must be followd by NUL
      if( ModemData.reg[REG_TELNET] && !modemTelnetState.sendBinary && buf[n-1] == 0x0d && !Serial.available() ) buf[n++] = 0;

      modemClient.write(buf, n);
    }
}


void loop()
{
  if( modemClient && modemClient.connected() )
    {
      // only relay data if not in command mode
      if( !modemCommandMode ) relayModemData();
    }
  else
    {
      // check whether connection to modem client was lost
      if( !modemCommandMode )
        {
          if( modemClient ) modemClient.stop();

          modemCommandMode = true;
          ModemData.reg[REG_CURLINESPEED] = 0;
          printModemResult(E_NOCARRIER);
        }
      else
        {
          prevRingTime = 0;
          ModemData.reg[1] = 0;
        }
    }

  if( modemCommandMode )
    {
      handleModemCommand();
    }
}
