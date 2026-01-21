package ascii

// byteRank is a frequency table for bytes based on corpus analysis.
// Lower rank = rarer byte = better candidate for rare-byte search.
// Derived from BYTE_FREQUENCIES table (corpus: CIA World Factbook,
// rustc source, Septuaginta). UTF-8 prefix bytes (0xC0-0xFF) forced to 255
// since continuation bytes are more discriminating.
var byteRank = [256]byte{
	// 0x00-0x0F: control characters (mostly rare)
	55, 52, 51, 50, 49, 48, 47, 46, 45, 103, 242, 66, 67, 229, 44, 43,
	// 0x10-0x1F: more control characters
	42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 56, 32, 31, 30, 29, 28,
	// 0x20-0x2F: space and punctuation
	255, // ' ' - most common
	148, // '!'
	164, // '"' - common in JSON
	149, // '#'
	136, // '$'
	160, // '%'
	155, // '&'
	173, // '\''
	221, // '('
	222, // ')'
	134, // '*'
	122, // '+'
	232, // ',' - common
	202, // '-'
	215, // '.'
	224, // '/'
	// 0x30-0x39: digits
	208, // '0'
	220, // '1'
	204, // '2'
	187, // '3'
	183, // '4'
	179, // '5'
	177, // '6'
	168, // '7'
	178, // '8'
	200, // '9'
	// 0x3A-0x40: more punctuation
	226, // ':' - common in JSON
	195, // ';'
	154, // '<'
	184, // '='
	174, // '>'
	126, // '?'
	120, // '@'
	// 0x41-0x5A: uppercase A-Z
	191, // 'A'
	157, // 'B'
	194, // 'C'
	170, // 'D'
	189, // 'E'
	162, // 'F'
	161, // 'G'
	150, // 'H'
	193, // 'I'
	142, // 'J'
	137, // 'K'
	171, // 'L'
	176, // 'M'
	185, // 'N'
	167, // 'O'
	186, // 'P'
	112, // 'Q' - rare
	175, // 'R'
	192, // 'S'
	188, // 'T'
	156, // 'U'
	140, // 'V'
	143, // 'W'
	123, // 'X' - rare
	133, // 'Y'
	128, // 'Z' - rare
	// 0x5B-0x60: brackets and punctuation
	147, // '['
	138, // '\\'
	146, // ']'
	114, // '^'
	223, // '_'
	151, // '`'
	// 0x61-0x7A: lowercase a-z
	249, // 'a'
	216, // 'b'
	238, // 'c'
	236, // 'd'
	253, // 'e' - most common letter
	227, // 'f'
	218, // 'g'
	230, // 'h'
	247, // 'i'
	135, // 'j' - rare
	180, // 'k'
	241, // 'l'
	233, // 'm'
	246, // 'n'
	244, // 'o'
	231, // 'p'
	139, // 'q' - rare
	245, // 'r'
	243, // 's'
	251, // 't'
	235, // 'u'
	201, // 'v'
	196, // 'w'
	240, // 'x'
	214, // 'y'
	152, // 'z' - rare
	// 0x7B-0x7F: braces and control
	182, // '{'
	205, // '|'
	181, // '}'
	127, // '~'
	27,  // DEL
	// 0x80-0xBF: UTF-8 continuation bytes (varied frequency in real UTF-8 text)
	212, 211, 210, 213, 228, 197, 169, 159, 131, 172, 105, 80, 98, 96, 97, 81,
	207, 145, 116, 115, 144, 130, 153, 121, 107, 132, 109, 110, 124, 111, 82, 108,
	118, 141, 113, 129, 119, 125, 165, 117, 92, 106, 83, 72, 99, 93, 65, 79,
	166, 237, 163, 199, 190, 225, 209, 203, 198, 217, 219, 206, 234, 248, 158, 239,
	// 0xC0-0xFF: UTF-8 prefix bytes (force to 255 = most common)
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
	255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
}

// toLower converts ASCII uppercase to lowercase.
func toLower(b byte) byte {
	if b >= 'A' && b <= 'Z' {
		return b + 0x20
	}
	return b
}
