#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/random.h>

#define DEVICE_NAME "fourinarow"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QuintenBallard");
MODULE_DESCRIPTION("Connect Four kernel module with reversed board display");

// == Globals == //
static dev_t dev_number;
static struct cdev c_dev;
static struct class *cl;

// 8x8 Board: row 0 is bottom, row 7 is top
static char board[8][8];

// Game state tracking
static bool game_in_progress = false;
static char current_player = 'R';  // 'R' or 'Y'
static char user_color     = 'R';  // either 'R' or 'Y'; set by RESET

#define BUF_LEN 512
static char msg_buffer[BUF_LEN];
static int msg_size = 0;

// == Prototypes == //
static int fourinarow_open(struct inode *i, struct file *f);
static int fourinarow_release(struct inode *i, struct file *f);
static ssize_t fourinarow_read(struct file *f, char __user *buf, size_t len, loff_t *off);
static ssize_t fourinarow_write(struct file *f, const char __user *buf, size_t len, loff_t *off);

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = fourinarow_open,
    .release = fourinarow_release,
    .read = fourinarow_read,
    .write = fourinarow_write,
};

// == Helper Functions == //

// Clear the board to '0'
static void reset_board(void)
{
    int r, c;
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            board[r][c] = '0';
        }
    }
}

// Convert column letter 'A'..'H' to 0..7
static int col_to_index(char col_char)
{
    if (col_char >= 'A' && col_char <= 'H') return col_char - 'A';
    if (col_char >= 'a' && col_char <= 'h') return col_char - 'a';
    return -1;
}

// Place a chip in the given column. Return 0 if success, 1 if col full, -1 if invalid col
static int drop_chip(char player, int col_idx)
{
    int row;  // declare at top for ISO C90
    if (col_idx < 0 || col_idx > 7) {
        return -1; // invalid
    }
    for (row = 0; row < 8; row++) {
        if (board[row][col_idx] == '0') {
            board[row][col_idx] = player;
            return 0; // success
        }
    }
    return 1; // column is full
}

// Check if the board is completely full => TIE
static bool board_is_full(void)
{
    int r, c;
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            if (board[r][c] == '0') return false;
        }
    }
    return true;
}

/* check_for_four:
 * Takes an initial cell (r, c) and a direction (dr, dc).
 * Checks if board[r][c] repeated 4 times in that direction is a winner.
 * Return 'R' or 'Y' if so, '0' otherwise.
 */
static char check_for_four(int r, int c, int dr, int dc)
{
    char start = board[r][c];
    int i;
    int rr = r, cc = c;

    if (start == '0') return '0';

    for (i = 1; i < 4; i++) {
        rr += dr;
        cc += dc;
        if (rr < 0 || rr >= 8 || cc < 0 || cc >= 8) return '0';
        if (board[rr][cc] != start) return '0';
    }
    // If we get here, all 4 match
    return start; // 'R' or 'Y'
}

/* check_game_over:
 * Checks entire board for a 4 in a row.
 * Return:
 *   'R'  if Red has 4 in a row
 *   'Y'  if Yellow has 4 in a row
 *   'T'  if tie (board full, no winner)
 *   '0'  otherwise
 */
static char check_game_over(void)
{
    int r, c;
    // 1. Horizontal checks
    // We can only start a horizontal 4 if c <= 4
    for (r = 0; r < 8; r++) {
        for (c = 0; c < 8; c++) {
            char winner;

            // horizontally (dr=0, dc=1)
            winner = check_for_four(r, c, 0, 1);
            if (winner != '0') return winner;

            // vertically (dr=1, dc=0)
            winner = check_for_four(r, c, 1, 0);
            if (winner != '0') return winner;

            // diag-down-right (dr=1, dc=1)
            winner = check_for_four(r, c, 1, 1);
            if (winner != '0') return winner;

            // diag-up-right (dr=-1, dc=1)
            winner = check_for_four(r, c, -1, 1);
            if (winner != '0') return winner;
        }
    }

    // 2. If no winner, check tie
    if (board_is_full()) {
        return 'T'; // tie
    }

    // 3. Otherwise, game not over
    return '0';
}

// Build the board string in msg_buffer
static void build_board_string(void)
{
    int r, c;
    msg_size = 0;
    memset(msg_buffer, 0, BUF_LEN);

    for (r = 7; r >= 0; r--) {
        for (c = 0; c < 8; c++) {
            msg_buffer[msg_size++] = board[r][c];
        }
        msg_buffer[msg_size++] = '\n';
    }
    // Null-terminate
    msg_buffer[msg_size] = '\0';
}

// == File Operation Handlers == //

static int fourinarow_open(struct inode *i, struct file *f)
{
    printk(KERN_INFO "fourinarow: open\n");
    return 0;
}

static int fourinarow_release(struct inode *i, struct file *f)
{
    printk(KERN_INFO "fourinarow: release\n");
    return 0;
}

static ssize_t fourinarow_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
    if (*off > 0) {
        // Already read
        return 0;
    }

    if (msg_size == 0) {
        // Nothing to read
        return 0;
    }

    if (copy_to_user(buf, msg_buffer, msg_size)) {
        return -EFAULT;
    }

    *off += msg_size;
    return msg_size;
}

static ssize_t fourinarow_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
    char input[BUF_LEN];
    int rc;
    memset(input, 0, BUF_LEN);

    memset(msg_buffer, 0, BUF_LEN);
    msg_size = 0;

    if (len >= BUF_LEN) {
        printk(KERN_WARNING "fourinarow: input too long!\n");
        return -EINVAL;
    }

    if (copy_from_user(input, buf, len)) {
        return -EFAULT;
    }
    input[len] = '\0'; // ensure null-terminated

    // Trim trailing newline if echo added it
    if (len > 0 && input[len-1] == '\n') {
        input[len-1] = '\0';
    }

    printk(KERN_INFO "fourinarow: Command received: %s\n", input);

    // == Parse Commands == //

    // 1. RESET R or RESET Y
    if (strncmp(input, "RESET R", 7) == 0) {
        reset_board();
        user_color = 'R';
        current_player = 'R';
        game_in_progress = true;
        snprintf(msg_buffer, BUF_LEN, "OK\n");
        msg_size = strlen(msg_buffer);
    }
    else if (strncmp(input, "RESET Y", 7) == 0) {
        reset_board();
        user_color = 'Y';
        current_player = 'Y';
        game_in_progress = true;
        snprintf(msg_buffer, BUF_LEN, "OK\n");
        msg_size = strlen(msg_buffer);
    }

    // 2. BOARD
    else if (strncmp(input, "BOARD", 5) == 0) {
        build_board_string();
        msg_size = strlen(msg_buffer);
    }

    // 3. DROPC <column>
    else if (strncmp(input, "DROPC ", 6) == 0) {
        if (!game_in_progress) {
            snprintf(msg_buffer, BUF_LEN, "NOGAME\n");
            msg_size = strlen(msg_buffer);
        }
        else if (current_player != user_color) {
            snprintf(msg_buffer, BUF_LEN, "OOT\n");
            msg_size = strlen(msg_buffer);
        }
        else {
            char col_char = input[6];
            int col_idx = col_to_index(col_char);
            if (col_idx < 0 || col_idx > 7) {
                snprintf(msg_buffer, BUF_LEN, "NOGAME\n"); 
                msg_size = strlen(msg_buffer);
            } else {
                rc = drop_chip(user_color, col_idx);
                if (rc == -1) {
                    snprintf(msg_buffer, BUF_LEN, "NOGAME\n");
                    msg_size = strlen(msg_buffer);
                } else if (rc == 1) {
                    snprintf(msg_buffer, BUF_LEN, "OK\n");
                    msg_size = strlen(msg_buffer);
                } else {
                    // success
                    char result = check_game_over();
                    if (result == user_color) {
                        snprintf(msg_buffer, BUF_LEN, "WIN\n");
                        msg_size = strlen(msg_buffer);
                        game_in_progress = false;
                    } else if (result == 'T') {
                        snprintf(msg_buffer, BUF_LEN, "TIE\n");
                        msg_size = strlen(msg_buffer);
                        game_in_progress = false;
                    } else {
                        snprintf(msg_buffer, BUF_LEN, "OK\n");
                        msg_size = strlen(msg_buffer);
                        // switch turn to computer
                        if (user_color == 'R')
                            current_player = 'Y';
                        else
                            current_player = 'R';
                    }
                }
            }
        }
    }

    // 4. CTURN
    else if (strncmp(input, "CTURN", 5) == 0) {
        if (!game_in_progress) {
            snprintf(msg_buffer, BUF_LEN, "NOGAME\n");
            msg_size = strlen(msg_buffer);
        }
        else if (current_player == user_color) {
            snprintf(msg_buffer, BUF_LEN, "OOT\n");
            msg_size = strlen(msg_buffer);
        }
        else {
            int tries = 0;
            int success = 0;
            while (tries < 100) {
                unsigned char rand_val;
                get_random_bytes(&rand_val, 1);
                int col_idx = rand_val % 8;

                rc = drop_chip(current_player, col_idx);
                if (rc == 0) {
                    success = 1;
                    break;
                }
                tries++;
            }
            if (!success) {
                // If can't place anywhere, maybe it's a tie or the board is full
                if (board_is_full()) {
                    snprintf(msg_buffer, BUF_LEN, "TIE\n");
                    msg_size = strlen(msg_buffer);
                    game_in_progress = false;
                } else {
                    snprintf(msg_buffer, BUF_LEN, "NOGAME\n");
                    msg_size = strlen(msg_buffer);
                }
            } else {
                // check if computer won or tie
                char result = check_game_over();
                if (result == current_player) {
                    snprintf(msg_buffer, BUF_LEN, "LOSE\n");
                    msg_size = strlen(msg_buffer);
                    game_in_progress = false;
                } else if (result == 'T') {
                    snprintf(msg_buffer, BUF_LEN, "TIE\n");
                    msg_size = strlen(msg_buffer);
                    game_in_progress = false;
                } else {
                    snprintf(msg_buffer, BUF_LEN, "OK\n");
                    msg_size = strlen(msg_buffer);
                    // turn goes back to user
                    current_player = user_color;
                }
            }
        }
    }

    // Unknown
    else {
        snprintf(msg_buffer, BUF_LEN, "UNKNOWN\n");
        msg_size = strlen(msg_buffer);
    }

    return len; 
}

// == Module Init/Exit == //

static int __init fourinarow_init(void)
{
    int ret;
    struct device *dev_ret;

    printk(KERN_INFO "fourinarow: Module init\n");

    if ((ret = alloc_chrdev_region(&dev_number, 0, 1, DEVICE_NAME)) < 0) {
        printk(KERN_ERR "fourinarow: Failed alloc_chrdev_region\n");
        return ret;
    }

    cdev_init(&c_dev, &fops);
    c_dev.owner = THIS_MODULE;
    if ((ret = cdev_add(&c_dev, dev_number, 1)) < 0) {
        printk(KERN_ERR "fourinarow: Failed cdev_add\n");
        unregister_chrdev_region(dev_number, 1);
        return ret;
    }

    // Create a class
    if (IS_ERR(cl = class_create(THIS_MODULE, "fourinarow_class"))) {
        printk(KERN_ERR "fourinarow: class_create error\n");
        cdev_del(&c_dev);
        unregister_chrdev_region(dev_number, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(dev_ret = device_create(cl, NULL, dev_number, NULL, DEVICE_NAME))) {
        printk(KERN_ERR "fourinarow: device_create error\n");
        class_destroy(cl);
        cdev_del(&c_dev);
        unregister_chrdev_region(dev_number, 1);
        return PTR_ERR(dev_ret);
    }

    // Initialize the board to empty
    reset_board();
    game_in_progress = false;
    current_player = 'R';
    user_color = 'R';

    printk(KERN_INFO "fourinarow: Device created at /dev/fourinarow\n");
    return 0;
}

static void __exit fourinarow_exit(void)
{
    device_destroy(cl, dev_number);
    class_destroy(cl);
    cdev_del(&c_dev);
    unregister_chrdev_region(dev_number, 1);

    printk(KERN_INFO "fourinarow: Module exit\n");
}

module_init(fourinarow_init);
module_exit(fourinarow_exit);
