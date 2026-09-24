#include <drives/ps2kbd.h>
#include <drives/tty.h>
#include <irq.h>

static char KeyboardRingBuffer[KEYBOARD_BUFFER_SIZE];//键盘缓冲区
static int key_count = 0;//可读键总数
static int buffer_head = 0;//写指针
static int buffer_tail = 0;//读指针

//修饰键状态
static _Bool shift_state = false;
static _Bool ctrl_state = false;
static _Bool alt_state = false;
static _Bool ext_prefix = false;

//io_getkey/io_getkey_noblock的转义序列暂存区（处理多字节输出）
static char io_esc_buf[5];
static int io_esc_len = 0;
static int io_esc_pos = 0;

//扫描码集1，普通ASCII字符映射表
static const char sc_normal[128] = {
	[0x01] = 0x1B,//Esc
	[0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',
	[0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8',
	[0x0A] = '9', [0x0B] = '0',
	[0x0C] = '-', [0x0D] = '=',
	[0x0E] = '\b',//Backspace
	[0x0F] = '\t',//Tab
	[0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',
	[0x14] = 't', [0x15] = 'y', [0x16] = 'u', [0x17] = 'i',
	[0x18] = 'o', [0x19] = 'p',
	[0x1A] = '[', [0x1B] = ']',
	[0x1C] = '\n',//Enter
	[0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',
	[0x22] = 'g', [0x23] = 'h', [0x24] = 'j', [0x25] = 'k',
	[0x26] = 'l',
	[0x27] = ';', [0x28] = '\'',
	[0x29] = '`',
	[0x2B] = '\\',
	[0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',
	[0x30] = 'b', [0x31] = 'n', [0x32] = 'm',
	[0x33] = ',', [0x34] = '.', [0x35] = '/',
	[0x39] = ' ',//Space
};

//扫描码集2，Shift+ASCII字符映射表
static const char sc_shifted[128] = {
	[0x01] = 0x1B,//Esc
	[0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$',
	[0x06] = '%', [0x07] = '^', [0x08] = '&', [0x09] = '*',
	[0x0A] = '(', [0x0B] = ')',
	[0x0C] = '_', [0x0D] = '+',
	[0x0E] = '\b',//Backspace
	[0x0F] = '\t',//Tab
	[0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R',
	[0x14] = 'T', [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I',
	[0x18] = 'O', [0x19] = 'P',
	[0x1A] = '{', [0x1B] = '}',
	[0x1C] = '\n',//Enter
	[0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
	[0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K',
	[0x26] = 'L',
	[0x27] = ':', [0x28] = '"',
	[0x29] = '~',
	[0x2B] = '|',
	[0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V',
	[0x30] = 'B', [0x31] = 'N', [0x32] = 'M',
	[0x33] = '<', [0x34] = '>', [0x35] = '?',
	[0x39] = ' ',//Space
};

static inline _Bool kbhit(void) {
    return (inb(PS2_KBD_STATUS_PORT) & 0x01);
}

//读取键值，非阻塞，有就读，没有就返回0
static uint8_t read_scan_code_nb(void) {
    if(kbhit()){
        return inb(PS2_KBD_KEY_PORT);//返回扫描码
    }else{
        return 0;
    }
}
//读取键值，阻塞，等待按键可读
static uint8_t read_scan_code(void) {
    while(!kbhit());//等待按键可读
    return inb(PS2_KBD_KEY_PORT);//返回扫描码
}

//Ctrl修饰，对字符应用标准ANSI控制字符映射（掩码bit5-6）
static char apply_ctrl(char ch){
	if (!ctrl_state && !alt_state)return ch;
	//Ctrl + 字母转控制字符 (0x01-0x1A)，Shift不影响Ctrl映射
	if (ch >= 'a' && ch <= 'z')return (char)(ch - 'a' + 1);
	if (ch >= 'A' && ch <= 'Z')return (char)(ch - 'A' + 1);
	if (ch == '@' || ch == '`' || ch == ' ' || ch == '2')return 0x00;//Ctrl + @ / ` / Space / 2，转0
	if (ch == '[' || ch == '{')return 0x1B;//Ctrl + [ / {，转ESC (0x1B)
	if (ch == '\\' || ch == '|')return 0x1C;//Ctrl + \ / |转FS (0x1C)
	if (ch == ']' || ch == '}')return 0x1D;//Ctrl + ] / }转GS (0x1D)
	if (ch == '^' || ch == '~')return 0x1E;//Ctrl + ^ / ~转RS (0x1E)
	if (ch == '_' || ch == '-')return 0x1F;//Ctrl + _ / -转US (0x1F)
	if (ch == '?' || ch == '/')return 0x7F;//Ctrl + ? / /转DEL (0x7F)
	return ch & 0x1F;//其他字符，转通用Ctrl掩码
}

//处理扫描码，将解析结果写入buf[0..3]，返回写入的字节数（0表示修饰键/无输出）
static int scan_to_buf(uint8_t sc, char *buf){
	//E0扩展码前缀
	if (sc == 0xE0) {
		ext_prefix = true;
		return 0;
	}
	//断码（释放）
	if (sc & 0x80) {
		uint8_t base = sc & 0x7F;
		if (ext_prefix) {
			ext_prefix = false;
		} else {
			if (base == KEY_LeftShift || base == KEY_RightShift)
				shift_state = false;
			else if (base == KEY_LeftCtrl)
				ctrl_state = false;
			else if (base == KEY_LeftAlt)
				alt_state = false;
		}
		return 0;
	}
	//E0扩展通码
	if (ext_prefix) {
		ext_prefix = false;
		if (sc == 0x1D) {ctrl_state = true; return 0;} //Right Ctrl
		if (sc == 0x38) {alt_state = true; return 0;}   //Right Alt
		//E0扩展键，VT100转义序列
		switch (sc) {
		case 0x47:buf[0]=0x1B;buf[1]='[';buf[2]='H';return 3; //Home
		case 0x48:buf[0]=0x1B;buf[1]='[';buf[2]='A';return 3; //Up
		case 0x49:buf[0]=0x1B;buf[1]='[';buf[2]='5';buf[3]='~';return 4; //PgUp
		case 0x4B:buf[0]=0x1B;buf[1]='[';buf[2]='D';return 3; //Left
		case 0x4D:buf[0]=0x1B;buf[1]='[';buf[2]='C';return 3; //Right
		case 0x4F:buf[0]=0x1B;buf[1]='[';buf[2]='F';return 3; //End
		case 0x50:buf[0]=0x1B;buf[1]='[';buf[2]='B';return 3; //Down
		case 0x51:buf[0]=0x1B;buf[1]='[';buf[2]='6';buf[3]='~';return 4; //PgDn
		case 0x52:buf[0]=0x1B;buf[1]='[';buf[2]='2';buf[3]='~';return 4; //Ins
		case 0x53:buf[0]=0x1B;buf[1]='[';buf[2]='3';buf[3]='~';return 4; //Del
		default:return 0;
		}
	}
	//修饰键通码，仅跟踪状态，不输出字符
	switch (sc) {
	case KEY_LeftShift:
	case KEY_RightShift:
		shift_state = true;
		return 0;
	case KEY_LeftCtrl:
		ctrl_state = true;
		return 0;
	case KEY_LeftAlt:
		alt_state = true;
		return 0;
	}
	//Esc单独按下，输出单字节ESC
	if (sc == 0x01) {
		buf[0] = 0x1B;
		return 1;
	}
	//F1-F10 (0x3B-0x44) → ESC O + 字母
	if (sc >= 0x3B && sc <= 0x44) {
		buf[0] = 0x1B;
		buf[1] = 'O';
		buf[2] = (char)('P' + (sc - 0x3B));//F1=P, F2=Q, ..., F10=Y
		return 3;
	}
	//F11 (0x57), F12 (0x58)
	if (sc == 0x57) {buf[0]=0x1B;buf[1]='[';buf[2]='2';buf[3]='3';buf[4]='~';return 5;} //F11
	if (sc == 0x58) {buf[0]=0x1B;buf[1]='[';buf[2]='2';buf[3]='4';buf[4]='~';return 5;} //F12
	//可打印字符
	char ch;
	ch = shift_state ? sc_shifted[sc] : sc_normal[sc];
	if (ch != 0) {
		//Alt修饰，转ESC前缀+字符
		if (alt_state) {
			buf[0] = 0x1B;
			buf[1] = apply_ctrl(ch);
			return 2;
		}
		buf[0] = apply_ctrl(ch);
		return 1;
	}
	return 0;//无映射键忽略
}

void KeyboardInit(){
	while (kbhit())inb(PS2_KBD_KEY_PORT);//清空硬件键盘缓冲区
    //重置缓冲区
	memset(KeyboardRingBuffer, 0, KEYBOARD_BUFFER_SIZE);
	key_count = 0;
	buffer_head = 0;
	buffer_tail = 0;
    //清除修饰键状态
	shift_state = false;
	ctrl_state = false;
	alt_state = false;
	ext_prefix = false;
    //清除转义序列暂存区
	io_esc_len = 0;
	io_esc_pos = 0;
}

_Bool kbdit(){
	return kbhit();
}

char io_getkey(){
	char c;
	//优先返回未消费完的转义序列字节
	if (io_esc_pos < io_esc_len)
		return io_esc_buf[io_esc_pos++];
	//从硬件读取并解析
	for (;;) {
		uint8_t sc = read_scan_code();
		int n = scan_to_buf(sc, io_esc_buf);
		if (n == 0)continue;
		if (n == 1)return io_esc_buf[0];
		//多字节转义序列：存入暂存区，返回首字节
		io_esc_len = n;
		io_esc_pos = 1;
		return io_esc_buf[0];
	}
}

char io_getkey_noblock(){
	char c;
	if (io_esc_pos < io_esc_len)return io_esc_buf[io_esc_pos++];
	uint8_t sc = read_scan_code_nb();
	if (sc == 0)return 0;
	int n = scan_to_buf(sc, io_esc_buf);
	if (n == 0) {
		//修饰键按下：尝试继续读
		sc = read_scan_code_nb();
		if (sc == 0)return 0;
		n = scan_to_buf(sc, io_esc_buf);
		if (n == 0)return 0;
	}
	if (n == 1)return io_esc_buf[0];
	io_esc_len = n;
	io_esc_pos = 1;
	return io_esc_buf[0];
}

void Keyboard_IRQ(){
	uint8_t sc = inb(PS2_KBD_KEY_PORT);
	char buf[5];
	int n = scan_to_buf(sc, buf);
	for (int i = 0; i < n; i++) {
		if(TTY_KeyInput(buf[i]))continue;//终端消费
		if (key_count >= KEYBOARD_BUFFER_SIZE)return;//满则丢弃所有未写入字节
		KeyboardRingBuffer[buffer_head] = buf[i];
		buffer_head = (buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
		key_count++;
	}
}

char GetKey(){
	char ch;
	while (key_count == 0)__asm__ volatile("hlt");
	cli();
	ch = KeyboardRingBuffer[buffer_tail];
	buffer_tail = (buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
	key_count--;
	sti();
	return ch;
}

char GetKey_NoBlock(){
	char ch;
	if (key_count == 0)return 0;
	cli();
	ch = KeyboardRingBuffer[buffer_tail];
	buffer_tail = (buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
	key_count--;
	sti();
	return ch;
}
