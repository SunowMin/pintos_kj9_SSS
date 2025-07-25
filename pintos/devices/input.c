#include "devices/input.h"
#include <debug.h>
#include "devices/intq.h"
#include "devices/serial.h"

/* Stores keys from the keyboard and serial port. */
static struct intq buffer;

/* Initializes the input buffer. */
void
input_init (void) {
	intq_init (&buffer);
}

/* Adds a key to the input buffer.
   Interrupts must be off and the buffer must not be full. */
void
input_putc (uint8_t key) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (!intq_full (&buffer));

	intq_putc (&buffer, key);
	serial_notify ();
}

/* Retrieves a key from the input buffer.
   If the buffer is empty, waits for a key to be pressed. 
   입력 버퍼에서 키를 가져옵니다. 버퍼가 비어있으면 키가 눌릴 때까지 대기합니다.*/
// 키보드 입력이 들어올 때까지 기다렸다가, 입력된 1바이트를 반환하는 함수
uint8_t
input_getc (void) {
	enum intr_level old_level;
	uint8_t key;

	// 현재 인터럽트 비활성화
	old_level = intr_disable ();
	// buffer에서 입력된 키 값을 1바이트 읽어옴
	key = intq_getc (&buffer);
	// 시리얼 포트에 알림을 보내 입력 처리 상태를 갱신함
	serial_notify ();
	// 원래의 인터럽트 상태로 복구
	intr_set_level (old_level);

	return key;
}

/* Returns true if the input buffer is full,
   false otherwise.
   Interrupts must be off. */
bool
input_full (void) {
	ASSERT (intr_get_level () == INTR_OFF);
	return intq_full (&buffer);
}
