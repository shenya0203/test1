// Copyright 2020-2021 The jdh99 Authors. All rights reserved.
// MD5哈希算法实现
// MD5算法将任意长度的数据转换为128位(16字节)的哈希值
// Authors: jdh99 <jdh821@163.com>

#include "../include/md5.h"
#include <string.h>

// Forward declarations for MD5 internal functions
static void MD5Transform(unsigned int state[4], unsigned char block[64]);
static void MD5Encode(unsigned char* output, unsigned int* input, unsigned int len);

// MD5算法中使用的四个基本函数
// 这些函数在MD5的四个轮次中分别使用
#define F(x,y,z) ((x & y) | (~x & z))    // 第1轮使用：选择函数，如果x为真则选择y，否则选择z
#define G(x,y,z) ((x & z) | (y & ~z))    // 第2轮使用：选择函数，如果z为真则选择x，否则选择y  
#define H(x,y,z) (x^y^z)                 // 第3轮使用：奇偶校验函数，三个输入的异或
#define I(x,y,z) (y ^ (x | ~z))          // 第4轮使用：混合函数

// 循环左移函数，将x向左循环移动n位
// MD5算法中需要进行大量的循环左移操作
#define ROTATE_LEFT(x,n) ((x << n) | (x >> (32-n)))

// MD5算法的四个轮函数宏定义
// 每个轮函数执行以下操作：a = b + ((a + 轮函数(b,c,d) + x + ac) <<< s)
// a,b,c,d: 四个32位状态变量
// x: 当前处理的32位数据块
// s: 循环左移的位数
// ac: 加法常数（避免对称性）

#define FF(a,b,c,d,x,s,ac) \
          { \
          a += F(b,c,d) + x + ac; \
          a = ROTATE_LEFT(a,s); \
          a += b; \
          }

#define GG(a,b,c,d,x,s,ac) \
          { \
          a += G(b,c,d) + x + ac; \
          a = ROTATE_LEFT(a,s); \
          a += b; \
          }

#define HH(a,b,c,d,x,s,ac) \
          { \
          a += H(b,c,d) + x + ac; \
          a = ROTATE_LEFT(a,s); \
          a += b; \
          }

#define II(a,b,c,d,x,s,ac) \
          { \
          a += I(b,c,d) + x + ac; \
          a = ROTATE_LEFT(a,s); \
          a += b; \
          }

// 静态全局MD5上下文（已注释掉）
//static MD5_CTX md5;

// MD5填充数据
// 用于消息填充，第一个字节是0x80，其余都是0
// MD5要求消息长度必须是512位的倍数，不足的部分用此数据填充
static unsigned char PADDING[] = {
    0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// 一次性计算MD5哈希值的便捷函数
// data: 输入数据指针
// len: 输入数据长度
// out: 输出MD5值，需要至少16字节空间
void MD5Calc(unsigned char* data, int len, unsigned char* out) {
    MD5_CTX md5;
    MD5Init(&md5);
    MD5Update(&md5, data, (unsigned int)len);
    MD5Final(&md5, out);
}

/**
 * 初始化MD5上下文
 * 设置MD5算法的初始状态值和计数器
 * @param context MD5上下文结构体指针
 */
void MD5Init(MD5_CTX* context) {
     // 初始化位计数器为0（记录已处理的位数）
     context->count[0] = 0;  // 低32位
     context->count[1] = 0;  // 高32位
     
     // 设置MD5算法的初始状态值（RFC 1321标准定义）
     // 这些是MD5算法规定的魔数，以小端序存储
     context->state[0] = 0x67452301;  // A
     context->state[1] = 0xEFCDAB89;  // B  
     context->state[2] = 0x98BADCFE;  // C
     context->state[3] = 0x10325476;  // D
}

/**
 * 更新MD5上下文，处理新的输入数据
 * 此函数可以被多次调用来处理大量数据
 * @param context MD5上下文结构体指针
 * @param input 输入数据指针
 * @param inputlen 输入数据长度（字节）
 */
void MD5Update(MD5_CTX* context, unsigned char *input, unsigned int inputlen) {
    unsigned int i = 0, index = 0, partlen = 0;
    
    // 计算当前缓冲区中已有数据的字节数（0-63）
    index = (context->count[0] >> 3) & 0x3F;
    
    // 计算还需要多少字节才能填满64字节的块
    partlen = 64 - index;
    
    // 更新总位数计数器
    context->count[0] += inputlen << 3;  // 将字节数转换为位数
    if(context->count[0] < (inputlen << 3)) {  // 检查是否溢出
       context->count[1]++;  // 低32位溢出，高32位加1
    }
    context->count[1] += inputlen >> 29;  // 处理输入长度的高位部分

    // 如果输入数据足够填满当前块
    if(inputlen >= partlen) {
       // 将数据复制到缓冲区，填满64字节块
       memcpy(&context->buffer[index], input, partlen);
       // 处理这个完整的64字节块
       MD5Transform(context->state,context->buffer);
       
       // 处理所有完整的64字节块
       for(i = partlen;i+64 <= inputlen;i+=64) {
           MD5Transform(context->state,&input[i]);
       }
       index = 0;  // 重置缓冲区索引
    } else {
        i = 0;  // 数据不足一个块，从输入开始处复制
    }
    
    // 将剩余数据复制到缓冲区
    memcpy(&context->buffer[index], &input[i], inputlen - i);
}

/**
 * 完成MD5计算，生成最终的哈希值
 * 进行最后的填充和长度追加，然后生成128位MD5值
 * @param context MD5上下文结构体指针  
 * @param digest 输出的16字节MD5哈希值
 */
void MD5Final(MD5_CTX* context, unsigned char digest[16]) 
{
    unsigned int index = 0, padlen = 0;
    unsigned char bits[8];  // 存储消息长度（位数）的8字节表示
    
    // 获取当前缓冲区中的数据字节数
    index = (context->count[0] >> 3) & 0x3F;
    
    // 计算需要填充的字节数
    // MD5要求：消息长度 ≡ 448 (mod 512)，即64字节块中留8字节存储原始长度
    padlen = (index < 56)?(56-index):(120-index);
    
    // 将64位消息长度转换为小端序8字节数组
    MD5Encode(bits,context->count,8);
    
    // 进行填充：先填充0x80后跟若干0x00
    MD5Update(context,PADDING,padlen);
    
    // 追加原始消息的位长度（小端序64位）
    MD5Update(context,bits,8);
    
    // 将最终的128位状态转换为16字节的MD5值
    MD5Encode(digest,context->state,16);
}

/**
 * 将32位整数数组编码为字节数组（小端序）
 * MD5算法使用小端字节序
 * @param output 输出字节数组
 * @param input 输入32位整数数组
 * @param len 要转换的字节数
 */
static void MD5Encode(unsigned char* output, unsigned int* input, unsigned int len) {
    unsigned int i = 0,j = 0;
    while(j < len) {
         // 将32位整数拆分为4个字节（小端序）
         output[j] = input[i] & 0xFF;           // 最低字节
         output[j+1] = (input[i] >> 8) & 0xFF;  // 次低字节
         output[j+2] = (input[i] >> 16) & 0xFF; // 次高字节  
         output[j+3] = (input[i] >> 24) & 0xFF; // 最高字节
         i++;
         j+=4;
    }
}

/**
 * 将字节数组解码为32位整数数组（小端序）
 * 用于将输入的字节数据转换为MD5算法所需的32位字格式
 * @param output 输出32位整数数组
 * @param input 输入字节数组
 * @param len 要转换的字节数
 */
void MD5Decode(unsigned int* output, unsigned char* input, unsigned int len) {
     unsigned int i = 0,j = 0;
     while(j < len) {
           // 将4个字节组合为一个32位整数（小端序）
           output[i] = (input[j]) |              // 最低字节
                       (input[j+1] << 8) |       // 次低字节
                       (input[j+2] << 16) |      // 次高字节
                       (input[j+3] << 24);       // 最高字节
           i++;
           j+=4;
     }
}

/**
 * MD5核心变换函数
 * 对一个64字节的数据块执行MD5变换，更新4个状态变量
 * 这是MD5算法的核心，包含4轮运算，每轮16步，共64步
 * @param state 4个32位状态变量数组（A,B,C,D）
 * @param block 64字节的输入数据块
 */
static void MD5Transform(unsigned int state[4], unsigned char block[64]) {
     // 保存当前状态到临时变量
     unsigned int a = state[0];
     unsigned int b = state[1];
     unsigned int c = state[2];
     unsigned int d = state[3];
     unsigned int x[64];  // 存储16个32位字的数组（实际只用前16个）

     // 将64字节块解码为16个32位字
     MD5Decode(x,block,64);
     
     /* 第1轮：使用F函数，进行16步运算 */
     /* 每一步的格式：FF(a, b, c, d, x[k], s, ac) */
     /* 其中k是消息字索引，s是左移位数，ac是加法常数 */
     FF(a, b, c, d, x[ 0], 7, 0xd76aa478); /* 1 */
     FF(d, a, b, c, x[ 1], 12, 0xe8c7b756); /* 2 */
     FF(c, d, a, b, x[ 2], 17, 0x242070db); /* 3 */
     FF(b, c, d, a, x[ 3], 22, 0xc1bdceee); /* 4 */
     FF(a, b, c, d, x[ 4], 7, 0xf57c0faf); /* 5 */
     FF(d, a, b, c, x[ 5], 12, 0x4787c62a); /* 6 */
     FF(c, d, a, b, x[ 6], 17, 0xa8304613); /* 7 */
     FF(b, c, d, a, x[ 7], 22, 0xfd469501); /* 8 */
     FF(a, b, c, d, x[ 8], 7, 0x698098d8); /* 9 */
     FF(d, a, b, c, x[ 9], 12, 0x8b44f7af); /* 10 */
     FF(c, d, a, b, x[10], 17, 0xffff5bb1); /* 11 */
     FF(b, c, d, a, x[11], 22, 0x895cd7be); /* 12 */
     FF(a, b, c, d, x[12], 7, 0x6b901122); /* 13 */
     FF(d, a, b, c, x[13], 12, 0xfd987193); /* 14 */
     FF(c, d, a, b, x[14], 17, 0xa679438e); /* 15 */
     FF(b, c, d, a, x[15], 22, 0x49b40821); /* 16 */

     /* 第2轮：使用G函数，进行16步运算 */
     /* 消息字的访问顺序有所不同 */
     GG(a, b, c, d, x[ 1], 5, 0xf61e2562); /* 17 */
     GG(d, a, b, c, x[ 6], 9, 0xc040b340); /* 18 */
     GG(c, d, a, b, x[11], 14, 0x265e5a51); /* 19 */
     GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa); /* 20 */
     GG(a, b, c, d, x[ 5], 5, 0xd62f105d); /* 21 */
     GG(d, a, b, c, x[10], 9,  0x2441453); /* 22 */
     GG(c, d, a, b, x[15], 14, 0xd8a1e681); /* 23 */
     GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8); /* 24 */
     GG(a, b, c, d, x[ 9], 5, 0x21e1cde6); /* 25 */
     GG(d, a, b, c, x[14], 9, 0xc33707d6); /* 26 */
     GG(c, d, a, b, x[ 3], 14, 0xf4d50d87); /* 27 */
     GG(b, c, d, a, x[ 8], 20, 0x455a14ed); /* 28 */
     GG(a, b, c, d, x[13], 5, 0xa9e3e905); /* 29 */
     GG(d, a, b, c, x[ 2], 9, 0xfcefa3f8); /* 30 */
     GG(c, d, a, b, x[ 7], 14, 0x676f02d9); /* 31 */
     GG(b, c, d, a, x[12], 20, 0x8d2a4c8a); /* 32 */

     /* 第3轮：使用H函数，进行16步运算 */
     HH(a, b, c, d, x[ 5], 4, 0xfffa3942); /* 33 */
     HH(d, a, b, c, x[ 8], 11, 0x8771f681); /* 34 */
     HH(c, d, a, b, x[11], 16, 0x6d9d6122); /* 35 */
     HH(b, c, d, a, x[14], 23, 0xfde5380c); /* 36 */
     HH(a, b, c, d, x[ 1], 4, 0xa4beea44); /* 37 */
     HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9); /* 38 */
     HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60); /* 39 */
     HH(b, c, d, a, x[10], 23, 0xbebfbc70); /* 40 */
     HH(a, b, c, d, x[13], 4, 0x289b7ec6); /* 41 */
     HH(d, a, b, c, x[ 0], 11, 0xeaa127fa); /* 42 */
     HH(c, d, a, b, x[ 3], 16, 0xd4ef3085); /* 43 */
     HH(b, c, d, a, x[ 6], 23,  0x4881d05); /* 44 */
     HH(a, b, c, d, x[ 9], 4, 0xd9d4d039); /* 45 */
     HH(d, a, b, c, x[12], 11, 0xe6db99e5); /* 46 */
     HH(c, d, a, b, x[15], 16, 0x1fa27cf8); /* 47 */
     HH(b, c, d, a, x[ 2], 23, 0xc4ac5665); /* 48 */

     /* 第4轮：使用I函数，进行16步运算 */
     II(a, b, c, d, x[ 0], 6, 0xf4292244); /* 49 */
     II(d, a, b, c, x[ 7], 10, 0x432aff97); /* 50 */
     II(c, d, a, b, x[14], 15, 0xab9423a7); /* 51 */
     II(b, c, d, a, x[ 5], 21, 0xfc93a039); /* 52 */
     II(a, b, c, d, x[12], 6, 0x655b59c3); /* 53 */
     II(d, a, b, c, x[ 3], 10, 0x8f0ccc92); /* 54 */
     II(c, d, a, b, x[10], 15, 0xffeff47d); /* 55 */
     II(b, c, d, a, x[ 1], 21, 0x85845dd1); /* 56 */
     II(a, b, c, d, x[ 8], 6, 0x6fa87e4f); /* 57 */
     II(d, a, b, c, x[15], 10, 0xfe2ce6e0); /* 58 */
     II(c, d, a, b, x[ 6], 15, 0xa3014314); /* 59 */
     II(b, c, d, a, x[13], 21, 0x4e0811a1); /* 60 */
     II(a, b, c, d, x[ 4], 6, 0xf7537e82); /* 61 */
     II(d, a, b, c, x[11], 10, 0xbd3af235); /* 62 */
     II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb); /* 63 */
     II(b, c, d, a, x[ 9], 21, 0xeb86d391); /* 64 */

     // 将变换结果加到原状态上（这样设计是为了防止逆向攻击）
     state[0] += a;
     state[1] += b;
     state[2] += c;
     state[3] += d;
}