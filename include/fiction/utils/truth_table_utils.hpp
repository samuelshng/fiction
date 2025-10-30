//
// Created by marcel on 05.04.22.
//

#ifndef FICTION_TRUTH_TABLE_UTILS_HPP
#define FICTION_TRUTH_TABLE_UTILS_HPP

#include <kitty/bit_operations.hpp>
#include <kitty/constructors.hpp>
#include <kitty/dynamic_truth_table.hpp>

#include <bitset>
#include <cassert>
#include <cstdint>
#include <vector>

namespace fiction
{

// NOLINTBEGIN(*-pointer-arithmetic)

/**
 * Creates and returns a truth table that implements the identity function in one variable.
 *
 * @return Identity function in one variable.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_id_tt() noexcept
{
    constexpr const uint64_t lit = 0x2;

    kitty::dynamic_truth_table table{1};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the negation in one variable.
 *
 * @return Negation in one variable.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_not_tt() noexcept
{
    constexpr const uint64_t lit = 0x1;

    kitty::dynamic_truth_table table{1};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the conjunction in two variables.
 *
 * @return Conjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_and_tt() noexcept
{
    constexpr const uint64_t lit = 0x8;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the disjunction in two variables.
 *
 * @return Disjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_or_tt() noexcept
{
    constexpr const uint64_t lit = 0xe;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the negated conjunction in two variables.
 *
 * @return Negated conjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_nand_tt() noexcept
{
    constexpr const uint64_t lit = 0x7;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the negated disjunction in two variables.
 *
 * @return Negated disjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_nor_tt() noexcept
{
    constexpr const uint64_t lit = 0x1;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the exclusive disjunction in two variables.
 *
 * @return Exclusive disjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_xor_tt() noexcept
{
    constexpr const uint64_t lit = 0x6;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the negated exclusive disjunction in two variables.
 *
 * @return Negated exclusive disjunction in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_xnor_tt() noexcept
{
    constexpr const uint64_t lit = 0x9;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the less-than function in two variables.
 *
 * @return Less-than function in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_lt_tt() noexcept
{
    constexpr const uint64_t lit = 0x2;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the greater-than function in two variables.
 *
 * @return Greater-than function in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_gt_tt() noexcept
{
    constexpr const uint64_t lit = 0x4;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the less-than-or-equal function in two variables.
 *
 * @return Less-than-or-equal function in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_le_tt() noexcept
{
    constexpr const uint64_t lit = 0x11;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the greater-than-or-equal function in two variables.
 *
 * @return Greater-than-or-equal function in two variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_ge_tt() noexcept
{
    constexpr const uint64_t lit = 0x13;

    kitty::dynamic_truth_table table{2};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the conjunction in three variables.
 *
 * @return Conjunction in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_and3_tt() noexcept
{
    constexpr const uint64_t lit = 0x80;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the XOR-AND function (a and (b xor c)) in three variables.
 *
 * @return XOR-AND in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_xor_and_tt() noexcept
{
    constexpr const uint64_t lit = 0x28;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the OR-AND function (a and (b or c)) in three variables.
 *
 * @return OR-AND in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_or_and_tt() noexcept
{
    constexpr const uint64_t lit = 0xa8;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the Onehot function (exactly one of a,b,c) in three variables.
 *
 * @return Onehot in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_onehot_tt() noexcept
{
    constexpr const uint64_t lit = 0x16;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the majority function in three variables.
 *
 * @return Majority function in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_maj_tt() noexcept
{
    constexpr const uint64_t lit = 0xe8;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the Gamble function (all or none of a,b,c) in three variables.
 *
 * @return Gamble in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_gamble_tt() noexcept
{
    constexpr const uint64_t lit = 0x81;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the Dot function (a xor (c or a and b)) in three variables.
 *
 * @return Dot function in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_dot_tt() noexcept
{
    constexpr const uint64_t lit = 0x52;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the ITE (MUX) function (if a then b else c) in three variables.
 *
 * @return ITE (MUX) in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_ite_tt() noexcept
{
    constexpr const uint64_t lit = 0xd8;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the AND-XOR function (a xor b and c) in three variables.
 *
 * @return AND-XOR in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_and_xor_tt() noexcept
{
    constexpr const uint64_t lit = 0x6a;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a truth table that implements the exclusive disjunction in three variables.
 *
 * @return Exclusive disjunction in three variables.
 */
[[nodiscard]] inline kitty::dynamic_truth_table create_xor3_tt() noexcept
{
    constexpr const uint64_t lit = 0x96;

    kitty::dynamic_truth_table table{3};
    kitty::create_from_words(table, &lit, &lit + 1);

    return table;
}
/**
 * Creates and returns a vector of truth tables for a double wire multi-output function.
 *
 * This function generates a vector of truth tables, each representing one of the outputs
 * of a double wire multi-output function in two variables. The function returns a vector containing
 * two truth tables.
 *
 * @return Vector of truth tables, each representing an output of the double wire function.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_double_wire_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1100";  // Output 1
    static constexpr const char* truth_table_string2 = "1010";  // Output 2

    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a crossing wire multi-output function.
 *
 * This function generates a vector of truth tables, each representing one of the outputs
 * of a crossing wire multi-output function in two variables. The function returns a vector containing
 * two truth tables.
 *
 * @return Vector of truth tables, each representing an output of the crossing wire function.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_crossing_wire_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1010";  // Output 1
    static constexpr const char* truth_table_string2 = "1100";  // Output 2

    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a multi-output function with two variables.
 *
 * This function generates a vector of truth tables, each representing one of the outputs
 * of a multi-output function in two variables.
 *
 * @return Vector of truth tables, each representing an output of the identity function.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string = "10";  // Output 1

    kitty::dynamic_truth_table table{1};

    kitty::create_from_binary_string(table, truth_table_string);

    return std::vector<kitty::dynamic_truth_table>{table, table};
}
/**
 * Creates and returns a vector of truth tables for a half adder multi-output function.
 *
 * This function generates a vector of truth tables, each representing one of the outputs
 * of a half adder multi-output function in two variables. The function returns a vector containing
 * two truth tables.
 *
 * @return Vector of truth tables, each representing an output of the half adder function.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_half_adder_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0110";  // Output 1
    static constexpr const char* truth_table_string2 = "1000";  // Output 2

    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a FALSE (constant 0) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a FALSE gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a FALSE gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_false_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0000";  // Output 1
    static constexpr const char* truth_table_string2 = "0000";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NOR gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NOR gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NOR gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_nor_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0001";  // Output 1
    static constexpr const char* truth_table_string2 = "0001";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NOT_A_AND_B (!A&B) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NOT_A_AND_B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NOT_A_AND_B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_not_a_and_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0010";  // Output 1
    static constexpr const char* truth_table_string2 = "0010";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NOT_A (!A) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NOT_A gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NOT_A gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_not_a_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0011";  // Output 1
    static constexpr const char* truth_table_string2 = "0011";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for an A_AND_NOT_B (A&!B) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * an A_AND_NOT_B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of an A_AND_NOT_B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_a_and_not_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0100";  // Output 1
    static constexpr const char* truth_table_string2 = "0100";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NOT_B (!B) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NOT_B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NOT_B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_not_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0101";  // Output 1
    static constexpr const char* truth_table_string2 = "0101";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a XOR gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a XOR gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a XOR gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_xor_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0110";  // Output 1
    static constexpr const char* truth_table_string2 = "0110";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NAND gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NAND gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NAND gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_nand_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "0111";  // Output 1
    static constexpr const char* truth_table_string2 = "0111";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for an AND gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * an AND gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of an AND gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_and_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1000";  // Output 1
    static constexpr const char* truth_table_string2 = "1000";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a XNOR gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a XNOR gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a XNOR gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_xnor_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1001";  // Output 1
    static constexpr const char* truth_table_string2 = "1001";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a B gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1010";  // Output 1
    static constexpr const char* truth_table_string2 = "1010";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a NOT_A_OR_B (!A|B) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a NOT_A_OR_B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a NOT_A_OR_B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_not_a_or_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1011";  // Output 1
    static constexpr const char* truth_table_string2 = "1011";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for an A gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * an A gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of an A gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_a_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1100";  // Output 1
    static constexpr const char* truth_table_string2 = "1100";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for an A_OR_NOT_B (A|!B) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * an A_OR_NOT_B gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of an A_OR_NOT_B gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_a_or_not_b_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1101";  // Output 1
    static constexpr const char* truth_table_string2 = "1101";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for an OR gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * an OR gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of an OR gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_or_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1110";  // Output 1
    static constexpr const char* truth_table_string2 = "1110";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}
/**
 * Creates and returns a vector of truth tables for a TRUE (constant 1) gate with fanout.
 * 
 * This function generates a vector of truth tables, each representing one of the outputs of
 * a TRUE gate with the output fanned out. The function returns a vector containing two truth
 * tables.
 * 
 * @return Vector of truth tables, each representing an output of a TRUE gate.
 */
[[nodiscard]] inline std::vector<kitty::dynamic_truth_table> create_true_fan_out_tt() noexcept
{
    static constexpr const char* truth_table_string1 = "1111";  // Output 1
    static constexpr const char* truth_table_string2 = "1111";  // Output 2
                                                                
    kitty::dynamic_truth_table table1{2};  // 2 input variables for Output 1
    kitty::dynamic_truth_table table2{2};  // 2 input variables for Output 2

    kitty::create_from_binary_string(table1, truth_table_string1);
    kitty::create_from_binary_string(table2, truth_table_string2);

    return std::vector<kitty::dynamic_truth_table>{table1, table2};
}

/**
 * This function evaluates the given multi-output truth table at the given input index.
 *
 * @param truth_tables The truth tables to evaluate.
 * @param current_input_index The index representing the current input pattern.
 * @return Output of the truth tables.
 */
[[nodiscard]] inline uint64_t evaluate_output(const std::vector<kitty::dynamic_truth_table>& truth_tables,
                                              const uint64_t current_input_index) noexcept
{
    assert(truth_tables.size() <= 64 && "Number of truth tables exceeds 64");

    std::bitset<64> bits{};
    for (auto i = 0u; i < truth_tables.size(); i++)
    {
        bits[i] = (kitty::get_bit(truth_tables[i], current_input_index) != 0u);
    }
    return bits.to_ulong();
}

// NOLINTEND(*-pointer-arithmetic)

}  // namespace fiction

#endif  // FICTION_TRUTH_TABLE_UTILS_HPP
