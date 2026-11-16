from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'main/interface/uart/at_uart_task.c').read_text()
function=source[source.index('static bool at_uart_route_raw'):source.index('static bool at_uart_wait_tx_done')]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
static bool raw,automatic,fault;static int g_at_cmd_port;static unsigned read_count,routed;
static bool air_ble_service_uart_active(void){return raw;}
static bool esp_at_bootrst_uart_is_data(void){return automatic;}
static bool air_ble_service_uart_route(const uint8_t*b,size_t n){(void)b;routed+=n;return true;}
static void air_ble_service_uart_fault(void){fault=true;}
static int uart_read_bytes(int p,uint8_t*b,size_t n,int t){(void)p;(void)t;memset(b,'X',n);read_count+=n;return n;}
FUNCTION
int main(void){
 assert(!at_uart_route_raw(300)&&!read_count);
 automatic=true;assert(at_uart_route_raw(300)&&read_count==300&&!routed&&!fault);
 raw=true;assert(at_uart_route_raw(258)&&routed==258&&!fault);
 automatic=false;raw=false;assert(!at_uart_route_raw(7));
 puts("Production UART routing: discard unsolicited startup bytes in automatic mode, raw forwarding intact PASS");
}
'''.replace('FUNCTION',function)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(code)
 gcc='D:/msys64/ucrt64/bin/gcc.exe' if os.name=='nt' else 'gcc'
 subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
