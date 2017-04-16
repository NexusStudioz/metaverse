/*=====================================================================
LoginDialog.cpp
---------------
=====================================================================*/
#include "LoginDialog.h"


#include <QtWidgets/QMessageBox>
#include <QtWidgets/QErrorMessage>
#include <QtWidgets/QPushButton>
#include <QtCore/QSettings>
#include <AESEncryption.h>
#include <Base64.h>
#include <Exception.h>
#include "../qt/QtUtils.h"


LoginDialog::LoginDialog(QSettings* settings_)
:	settings(settings_)
{
	setupUi(this);

	// Load main window geometry and state
	this->restoreGeometry(settings->value("LoginDialog/geometry").toByteArray());

	this->usernameLineEdit->setText(settings->value("LoginDialog/username").toString());
	this->passwordLineEdit->setText(QtUtils::toQString(decryptPassword(QtUtils::toStdString(settings->value("LoginDialog/password").toString()))));

	this->buttonBox->button(QDialogButtonBox::Ok)->setText("Log in");

	connect(this->buttonBox, SIGNAL(accepted()), this, SLOT(accepted()));
}


LoginDialog::~LoginDialog()
{
	settings->setValue("LoginDialog/geometry", saveGeometry());
}


void LoginDialog::accepted()
{
	settings->setValue("LoginDialog/username", this->usernameLineEdit->text());
	settings->setValue("LoginDialog/password", QtUtils::toQString(encryptPassword(QtUtils::toStdString(this->passwordLineEdit->text()))));
}


const std::string LoginDialog::decryptPassword(const std::string& cyphertext_base64)
{
	try
	{
		// Decode base64 to raw bytes.
		std::vector<unsigned char> cyphertex_binary;
		Base64::decode(cyphertext_base64, cyphertex_binary);

		// AES decrypt
		const std::string key = "RHJKEF_ZAepxYxYkrL3c6rWD";
		const std::string salt = "P6A3uZ4P";
		AESEncryption aes((const unsigned char*)key.c_str(), (int)key.size(), (const unsigned char*)salt.c_str());
		std::vector<unsigned char> plaintext_v = aes.decrypt(cyphertex_binary);

		// Convert to std::string
		std::string plaintext(plaintext_v.size(), '\0');
		if(!plaintext_v.empty())
			std::memcpy(&plaintext[0], plaintext_v.data(), plaintext_v.size());
		return plaintext;
	}
	catch(Indigo::Exception&)
	{
		return "";
	}
}


const std::string LoginDialog::encryptPassword(const std::string& password_plaintext)
{
	try
	{
		// Copy password to vector
		std::vector<unsigned char> plaintext_v(password_plaintext.size());
		if(!plaintext_v.empty())
			std::memcpy(&plaintext_v[0], password_plaintext.data(), password_plaintext.size());

		// AES encrypt
		const std::string key = "RHJKEF_ZAepxYxYkrL3c6rWD";
		const std::string salt = "P6A3uZ4P";
		AESEncryption aes((const unsigned char*)key.c_str(), (int)key.size(), (const unsigned char*)salt.c_str());
		std::vector<unsigned char> cyphertext = aes.encrypt(plaintext_v);

		// Encode in base64.
		std::string cyphertext_base64;
		Base64::encode(cyphertext.data(), cyphertext.size(), cyphertext_base64);

		return cyphertext_base64;
	}
	catch(Indigo::Exception&)
	{
		return "";
	}
}