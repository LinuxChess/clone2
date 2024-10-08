﻿#if defined (EVAL_LEARN)

#include "sfen_packer.h"

#include "packed_sfen.h"

#include "misc.h"
#include "position.h"

#include <sstream>
#include <fstream>
#include <cstring> // std::memset()

using namespace std;

namespace Learner {

  // Class that handles bitstream
  // useful when doing aspect encoding
  struct BitStream
  {
    // Set the memory to store the data in advance.
    // Assume that memory is cleared to 0.
    void set_data(std::uint8_t* data_) { data = data_; reset(); }

    // Get the pointer passed in set_data().
    uint8_t* get_data() const { return data; }

    // Get the cursor.
    int get_cursor() const { return bit_cursor; }

    // reset the cursor
    void reset() { bit_cursor = 0; }

    // Write 1bit to the stream.
    // If b is non-zero, write out 1. If 0, write 0.
    void write_one_bit(int b)
    {
      if (b)
        data[bit_cursor / 8] |= 1 << (bit_cursor & 7);

      ++bit_cursor;
    }

    // Get 1 bit from the stream.
    int read_one_bit()
    {
      int b = (data[bit_cursor / 8] >> (bit_cursor & 7)) & 1;
      ++bit_cursor;

      return b;
    }

    // write n bits of data
    // Data shall be written out from the lower order of d.
    void write_n_bit(int d, int n)
    {
      for (int i = 0; i <n; ++i)
        write_one_bit(d & (1 << i));
    }

    // read n bits of data
    // Reverse conversion of write_n_bit().
    int read_n_bit(int n)
    {
      int result = 0;
      for (int i = 0; i < n; ++i)
        result |= read_one_bit() ? (1 << i) : 0;

      return result;
    }

  private:
    // Next bit position to read/write.
    int bit_cursor;

    // data entity
    std::uint8_t* data;
  };

  // Class for compressing/decompressing sfen
  // sfen can be packed to 256bit (32bytes) by Huffman coding.
  // This is proven by mini. The above is Huffman coding.
  //
  // Internal format = 1-bit turn + 7-bit king position *2 + piece on board (Huffman coding) + hand piece (Huffman coding)
  // Side to move (White = 0, Black = 1) (1bit)
  // White King Position (6 bits)
  // Black King Position (6 bits)
  // Huffman Encoding of the board
  // Castling availability (1 bit x 4)
  // En passant square (1 or 1 + 6 bits)
  // Rule 50 (6 bits)
  // Game play (8 bits)
  //
  // TODO(someone): Rename SFEN to FEN.
  //
  struct SfenPacker
  {
    void pack(const Position& pos);

    // sfen packed by pack() (256bit = 32bytes)
    // Or sfen to decode with unpack()
    uint8_t *data; // uint8_t[32];

    BitStream stream;

    // Output the board pieces to stream.
    void write_board_piece_to_stream(Piece pc);

    // Read one board piece from stream
    Piece read_board_piece_from_stream();
  };


  // Huffman coding
  // * is simplified from mini encoding to make conversion easier.
  //
  // 1 box on the board (other than NO_PIECE) = 2 to 6 bits (+ 1-bit flag + 1-bit forward and backward)
  // 1 piece of hand piece = 1-5bit (+ 1-bit flag + 1bit ahead and behind)
  //
  // empty xxxxx0 + 0 (none)
  // step xxxx01 + 2 xxxx0 + 2
  // incense xx0011 + 2 xx001 + 2
  // Katsura xx1011 + 2 xx101 + 2
  // silver xx0111 + 2 xx011 + 2
  // Gold x01111 + 1 x0111 + 1 // Gold is valid and has no flags.
  // corner 011111 + 2 01111 + 2
  // Fly 111111 + 2 11111 + 2
  //
  // Assuming all pieces are on the board,
  // Sky 81-40 pieces = 41 boxes = 41bit
  // Walk 4bit*18 pieces = 72bit
  // Incense 6bit*4 pieces = 24bit
  // Katsura 6bit*4 pieces = 24bit
  // Silver 6bit*4 pieces = 24bit
  // Gold 6bit* 4 pieces = 24bit
  // corner 8bit* 2 pieces = 16bit
  // Fly 8bit* 2 pieces = 16bit
  // -------
  // 241bit + 1bit (turn) + 7bit × 2 (King's position after) = 256bit
  //
  // When the piece on the board moves to the hand piece, the piece on the board becomes empty, so the box on the board can be expressed with 1 bit,
  // Since the hand piece can be expressed by 1 bit less than the piece on the board, the total number of bits does not change in the end.
  // Therefore, in this expression, any aspect can be expressed by this bit number.
  // It is a hand piece and no flag is required, but if you include this, the bit number of the piece on the board will be -1
  // Since the total number of bits can be fixed, we will include this as well.

  // Huffman Encoding
  //
  // Empty  xxxxxxx0
  // Pawn   xxxxx001 + 1 bit (Side to move)
  // Knight xxxxx011 + 1 bit (Side to move)
  // Bishop xxxxx101 + 1 bit (Side to move)
  // Rook   xxxxx111 + 1 bit (Side to move)

  struct HuffmanedPiece
  {
    int code; // how it will be coded
    int bits; // How many bits do you have
  };

  constexpr HuffmanedPiece huffman_table[] =
  {
    {0b0000,1}, // NO_PIECE
    {0b0001,4}, // PAWN
    {0b0011,4}, // KNIGHT
    {0b0101,4}, // BISHOP
    {0b0111,4}, // ROOK
    {0b1001,4}, // QUEEN
  };

  // Pack sfen and store in data[32].
  void SfenPacker::pack(const Position& pos)
  {
  // cout << pos;

    memset(data, 0, 32 /* 256bit */);
    stream.set_data(data);

    // turn
    // Side to move.
    stream.write_one_bit((int)(pos.side_to_move()));

    // 7-bit positions for leading and trailing balls
    // White king and black king, 6 bits for each.
    for(auto c: Colors)
      stream.write_n_bit(pos.king_square(c), 6);

    // Write the pieces on the board other than the kings.
    for (Rank r = RANK_8; r >= RANK_1; --r)
    {
      for (File f = FILE_A; f <= FILE_H; ++f)
      {
        Piece pc = pos.piece_on(make_square(f, r));
        if (type_of(pc) == KING)
          continue;
        write_board_piece_to_stream(pc);
      }
    }

    // TODO(someone): Support chess960.
    stream.write_one_bit(pos.can_castle(WHITE_OO));
    stream.write_one_bit(pos.can_castle(WHITE_OOO));
    stream.write_one_bit(pos.can_castle(BLACK_OO));
    stream.write_one_bit(pos.can_castle(BLACK_OOO));

    if (pos.ep_square() == SQ_NONE) {
      stream.write_one_bit(0);
    }
    else {
      stream.write_one_bit(1);
      stream.write_n_bit(static_cast<int>(pos.ep_square()), 6);
    }

    stream.write_n_bit(pos.state()->rule50, 6);

    stream.write_n_bit(1 + (pos.game_ply()-(pos.side_to_move() == BLACK)) / 2, 8);

    assert(stream.get_cursor() <= 256);
  }

  // Output the board pieces to stream.
  void SfenPacker::write_board_piece_to_stream(Piece pc)
  {
    // piece type
    PieceType pr = type_of(pc);
    auto c = huffman_table[pr];
    stream.write_n_bit(c.code, c.bits);

    if (pc == NO_PIECE)
      return;

    // first and second flag
    stream.write_one_bit(color_of(pc));
  }

  // Read one board piece from stream
  Piece SfenPacker::read_board_piece_from_stream()
  {
    PieceType pr = NO_PIECE_TYPE;
    int code = 0, bits = 0;
    while (true)
    {
      code |= stream.read_one_bit() << bits;
      ++bits;

      assert(bits <= 6);

      for (pr = NO_PIECE_TYPE; pr <KING; ++pr)
        if (huffman_table[pr].code == code
          && huffman_table[pr].bits == bits)
          goto Found;
    }
  Found:;
    if (pr == NO_PIECE_TYPE)
      return NO_PIECE;

    // first and second flag
    Color c = (Color)stream.read_one_bit();

    return make_piece(c, pr);
  }

  int set_from_packed_sfen(Position& pos, const PackedSfen& sfen, StateInfo* si, Thread* th, bool mirror)
  {
    SfenPacker packer;
    auto& stream = packer.stream;

    // TODO: separate streams for writing and reading. Here we actually have to
    // const_cast which is not safe in the long run.
    stream.set_data(const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(&sfen)));

    pos.clear();
    std::memset(si, 0, sizeof(StateInfo));
    std::fill_n(&pos.pieceList[0][0], sizeof(pos.pieceList) / sizeof(Square), SQ_NONE);
    pos.st = si;

    // Active color
    pos.sideToMove = (Color)stream.read_one_bit();

    pos.pieceList[W_KING][0] = SQUARE_NB;
    pos.pieceList[B_KING][0] = SQUARE_NB;

    // First the position of the ball
    if (mirror)
    {
      for (auto c : Colors)
        pos.board[flip_file((Square)stream.read_n_bit(6))] = make_piece(c, KING);
    }
    else
    {
      for (auto c : Colors)
        pos.board[stream.read_n_bit(6)] = make_piece(c, KING);
    }

    // Piece placement
    for (Rank r = RANK_8; r >= RANK_1; --r)
    {
      for (File f = FILE_A; f <= FILE_H; ++f)
      {
        auto sq = make_square(f, r);
        if (mirror) {
          sq = flip_file(sq);
        }

        // it seems there are already balls
        Piece pc;
        if (type_of(pos.board[sq]) != KING)
        {
          assert(pos.board[sq] == NO_PIECE);
          pc = packer.read_board_piece_from_stream();
        }
        else
        {
          pc = pos.board[sq];
          // put_piece() will catch ASSERT unless you remove it all.
          pos.board[sq] = NO_PIECE;
        }

        // There may be no pieces, so skip in that case.
        if (pc == NO_PIECE)
          continue;

        pos.put_piece(Piece(pc), sq);

        if (stream.get_cursor()> 256)
          return 1;

        //assert(stream.get_cursor() <= 256);
      }
    }

    // Castling availability.
    // TODO(someone): Support chess960.
    pos.st->castlingRights = 0;
    if (stream.read_one_bit()) {
      Square rsq;
      for (rsq = relative_square(WHITE, SQ_H1); pos.piece_on(rsq) != W_ROOK; --rsq) {}
      pos.set_castling_right(WHITE, rsq);
    }
    if (stream.read_one_bit()) {
      Square rsq;
      for (rsq = relative_square(WHITE, SQ_A1); pos.piece_on(rsq) != W_ROOK; ++rsq) {}
      pos.set_castling_right(WHITE, rsq);
    }
    if (stream.read_one_bit()) {
      Square rsq;
      for (rsq = relative_square(BLACK, SQ_H1); pos.piece_on(rsq) != B_ROOK; --rsq) {}
      pos.set_castling_right(BLACK, rsq);
    }
    if (stream.read_one_bit()) {
      Square rsq;
      for (rsq = relative_square(BLACK, SQ_A1); pos.piece_on(rsq) != B_ROOK; ++rsq) {}
      pos.set_castling_right(BLACK, rsq);
    }

    // En passant square. Ignore if no pawn capture is possible
    if (stream.read_one_bit()) {
      Square ep_square = static_cast<Square>(stream.read_n_bit(6));
      if (mirror) {
        ep_square = flip_file(ep_square);
      }
      pos.st->epSquare = ep_square;

      if (!(pos.attackers_to(pos.st->epSquare) & pos.pieces(pos.sideToMove, PAWN))
        || !(pos.pieces(~pos.sideToMove, PAWN) & (pos.st->epSquare + pawn_push(~pos.sideToMove))))
        pos.st->epSquare = SQ_NONE;
    }
    else {
      pos.st->epSquare = SQ_NONE;
    }

    // Halfmove clock
    pos.st->rule50 = static_cast<Square>(stream.read_n_bit(6));

    // Fullmove number
    pos.gamePly = static_cast<Square>(stream.read_n_bit(8));

    // Convert from fullmove starting from 1 to gamePly starting from 0,
    // handle also common incorrect FEN with fullmove = 0.
    pos.gamePly = std::max(2 * (pos.gamePly - 1), 0) + (pos.sideToMove == BLACK);

    assert(stream.get_cursor() <= 256);

    pos.chess960 = false;
    pos.thisThread = th;
    pos.set_state(pos.st);

    assert(pos.pos_is_ok());

    return 0;
  }

  PackedSfen sfen_pack(Position& pos)
  {
    PackedSfen sfen;

    SfenPacker sp;
    sp.data = (uint8_t*)&sfen;
    sp.pack(pos);

    return sfen;
  }
}


#endif // USE_SFEN_PACKER
